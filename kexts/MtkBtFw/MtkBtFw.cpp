// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// MtkBtFw: loads the Bluetooth firmware into the MediaTek MT7925 (USB 0489:e11e on the GA403WM), then gets out of the way.
//
// The chip enumerates as a standard USB Bluetooth controller (class e0/01/01) but answers HCI only after its RAM code has been
// downloaded. macOS has no loader for MediaTek parts, so bluetoothd finds no controller. This is the procedure of Linux
// drivers/bluetooth/btmtk.c (btmtk_usb_setup -> btmtk_setup_firmware_79xx, btmtk_usb_hci_wmt_sync):
//   * WMT commands are HCI vendor command 0xfc6f sent as a class request on the default control endpoint; the WMT event comes
//     back through a vendor IN request (bRequest 1, wValue 0x30) that has to be polled until it returns data.
//   * per firmware section: a PATCH_DWNLD query with the section's 52-byte spec (answer: download / already there / busy), then
//     the section in 250-byte blocks (flag 1 first, 2 middle, 3 last).
//   * endpoint reset option, then WMT FUNC_CTRL 1 (Bluetooth on).
// The interface is opened only for that and closed again, so bluetoothd (with BlueToolFixup) can use the controller. A section
// that is already loaded (warm reboot from Windows/Linux) is skipped by the chip's answer, as in Linux.
//
// Recovery (Linux: btmtk_usb_setup retries a failed btmtk_setup_firmware_79xx / a timed-out FUNC_CTRL through btmtk_reset_sync,
// whose USB callback btusb_mtk_reset runs btmtk_usb_subsys_reset and then sets the device up again from the start):
//   * a missing WMT event, a failed block or a bad status runs the MT7925 subsystem reset and restarts from section 0, up to
//     MAX_ATTEMPTS downloads in all.
//   * no answer to the very first PATCH_DWNLD query (no WMT event at all yet) is its own case: the RAM code may already be
//     running from the previous boot and no longer answer WMT. An HCI Read Local Version on the normal HCI path tells the two
//     apart; if the controller answers it is left as it is, otherwise the reset path is taken.
//
// IOLog of these kexts does not reach dmesg or the unified log on this machine: the result is in the registry,
//   ioreg -rc MtkBtFw -l | grep -E '"bt-state"|"bt-log"|"bt-attempts"'
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <kern/thread.h>
#include <stdarg.h>
#include <string.h>

extern "C" {
extern const uint8_t mtk_bt_fw[], mtk_bt_fw_end[];
}

#define FW_HDR_SIZE       32
#define FW_GD_SIZE        64
#define FW_SEC_MAP_SIZE   64
#define SEC_COMMON_SIZE   12
#define SEC_SEND_SIZE     52

enum { WMT_PATCH_DWNLD = 0x1, WMT_FUNC_CTRL = 0x6 };
// registers (btmtk.h)
#define REG_DEV_ID           0x70010200      // MTK_BT_READ_DEV_ID
#define REG_FW_VER           0x80021004
#define REG_EP_RST_OPT       0x74011890      // MTK_EP_RST_OPT
#define EP_RST_IN_OUT_OPT    0x00010001      // MTK_EP_RST_IN_OUT_OPT
#define REG_BT_MISC          0x70002510      // MTK_BT_MISC
#define BT_RST_DONE          0x00000100      // MTK_BT_RST_DONE
#define REG_RESET_CONNV3     0x70028610      // MTK_BT_RESET_REG_CONNV3
#define REG_UDMA_INT_STA_BT  0x74000024      // MTK_UDMA_INT_STA_BT
#define REG_UDMA_INT_STA_BT1 0x74000308      // MTK_UDMA_INT_STA_BT1

#define MAX_ATTEMPTS      4                   // downloads in all (the first one plus up to 3 after a subsystem reset)
#define WMT_TIMEOUT_MS    10000               // HCI_INIT_TIMEOUT, what btmtk_usb_hci_wmt_sync waits for a WMT event

enum { WMT_OK = 0, WMT_SEND_FAILED = -1, WMT_NO_EVENT = -2 };
enum DlResult { DL_OK, DL_FAILED, DL_NO_FIRST_ANSWER };

static void startThread(void (*fn)(void *), void *arg)
{
    thread_t th = nullptr;
    if (kernel_thread_start((thread_continue_t)fn, arg, &th) == KERN_SUCCESS) thread_deallocate(th);
}

static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint64_t nowMs()
{
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000000ull;
}

class MtkBtFw : public IOService {
    OSDeclareDefaultStructors(MtkBtFw)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
private:
    IOUSBHostInterface *intf = nullptr;
    IOUSBHostPipe *intrPipe = nullptr;
    IOBufferMemoryDescriptor *intrBuf = nullptr;
    volatile bool intrStop = false, intrBusy = false;
    volatile uint32_t intrPackets = 0;
    uint8_t intrFirst[8] = {0};                     // first interrupt packet, for the log
    volatile uint32_t intrFirstLen = 0;
    volatile bool hciWait = false, hciGot = false;  // HCI Read Local Version probe
    uint8_t hciEv[16] = {0};
    bool wmtAnswered = false;                       // any WMT event seen in this download attempt
    uint8_t *cmd = nullptr, *ev = nullptr;          // control transfer buffers (wired)
    char log[2400] = {0};
    size_t loglen = 0;

    static void worker(void *arg);
    void run();
    void say(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void state(const char *s) { setProperty("bt-state", s); }
    IOReturn ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, uint16_t len, uint32_t *done);
    bool regRead(uint32_t reg, uint32_t *val);
    bool uhwRead(uint32_t reg, uint32_t *val);
    bool uhwWrite(uint32_t reg, uint32_t val);
    int wmt(uint8_t op, uint8_t flag, const uint8_t *data, uint16_t dlen, uint8_t *evFlag);
    void drainWmt();
    DlResult download();
    bool subsysReset();
    bool hciAlive();
    void intrArm();
    static void intrDone(void *owner, void *param, IOReturn status, uint32_t bytes);
};

OSDefineMetaClassAndStructors(MtkBtFw, IOService)

void MtkBtFw::say(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOLog("MtkBtFw: %s\n", line);
    size_t n = strlen(line);
    if (loglen + n + 2 >= sizeof(log)) return;      // keep the beginning: that is where things go wrong
    memcpy(log + loglen, line, n);
    loglen += n;
    log[loglen++] = ';';
    log[loglen] = 0;
    setProperty("bt-log", log);
}

IOReturn MtkBtFw::ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, uint16_t len, uint32_t *done)
{
    StandardUSB::DeviceRequest r;
    r.bmRequestType = type;
    r.bRequest = req;
    r.wValue = value;
    r.wIndex = index;
    r.wLength = len;
    uint32_t n = 0;
    IOReturn ret = intf->deviceRequest(r, len ? buf : nullptr, n, 1000);
    if (done) *done = n;
    return ret;
}

// btmtk_usb_reg_read: vendor IN, bRequest 0x63
bool MtkBtFw::regRead(uint32_t reg, uint32_t *val)
{
    uint32_t n = 0;
    IOReturn r = ctrl(0xc0, 0x63, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xffff), ev, 4, &n);
    if (r != kIOReturnSuccess || n != 4) { say("reg %08x read failed %08x n=%u", reg, r, n); return false; }
    *val = le32(ev);
    return true;
}

// btmtk_usb_uhw_reg_read: request type 0xde, bRequest 0x01
bool MtkBtFw::uhwRead(uint32_t reg, uint32_t *val)
{
    uint32_t n = 0;
    IOReturn r = ctrl(0xde, 0x01, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xffff), ev, 4, &n);
    if (r != kIOReturnSuccess || n != 4) { say("uhw %08x read failed %08x n=%u", reg, r, n); return false; }
    *val = le32(ev);
    return true;
}

// btmtk_usb_uhw_reg_write: request type 0x5e, bRequest 0x02
bool MtkBtFw::uhwWrite(uint32_t reg, uint32_t val)
{
    cmd[0] = (uint8_t)val; cmd[1] = (uint8_t)(val >> 8); cmd[2] = (uint8_t)(val >> 16); cmd[3] = (uint8_t)(val >> 24);
    IOReturn r = ctrl(0x5e, 0x02, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xffff), cmd, 4, nullptr);
    if (r != kIOReturnSuccess) { say("uhw %08x write failed %08x", reg, r); return false; }
    return true;
}

// One WMT command and its event (btmtk_usb_hci_wmt_sync). Returns WMT_OK and the event's flag byte, WMT_SEND_FAILED or
// WMT_NO_EVENT after WMT_TIMEOUT_MS.
int MtkBtFw::wmt(uint8_t op, uint8_t flag, const uint8_t *data, uint16_t dlen, uint8_t *evFlag)
{
    uint32_t hlen = 5u + dlen;
    if (hlen > 255) return WMT_SEND_FAILED;
    cmd[0] = 0x6f; cmd[1] = 0xfc; cmd[2] = (uint8_t)hlen;           // HCI command 0xfc6f
    cmd[3] = 1; cmd[4] = op; cmd[5] = (uint8_t)(dlen + 1); cmd[6] = (uint8_t)((dlen + 1) >> 8); cmd[7] = flag;
    if (dlen) memcpy(cmd + 8, data, dlen);
    IOReturn r = ctrl(0x20, 0x00, 0, 0, cmd, (uint16_t)(3 + hlen), nullptr);
    if (r != kIOReturnSuccess) { say("wmt op %u send failed %08x", op, r); return WMT_SEND_FAILED; }
    uint64_t deadline = nowMs() + WMT_TIMEOUT_MS;
    do {
        uint32_t n = 0;
        r = ctrl(0xc0, 0x01, 0x30, 0, ev, 64, &n);
        if (r == kIOReturnSuccess && n >= 7) {
            if (ev[0] != 0xe4 || ev[3] != op) { say("wmt op %u: other event %02x op %u n=%u", op, ev[0], ev[3], n); continue; }
            if (evFlag) *evFlag = ev[6];
            wmtAnswered = true;
            return WMT_OK;
        }
        IODelay(500);                                               // btmtk_usb_wmt_recv: 500 us between polls
    } while (nowMs() < deadline);
    say("wmt op %u flag %u: no event", op, flag);
    return WMT_NO_EVENT;
}

// Reads and drops WMT events still queued (a late answer to a command that timed out) so the next command does not take one
// of them for its own.
void MtkBtFw::drainWmt()
{
    uint32_t dropped = 0;
    for (int empty = 0, i = 0; empty < 20 && i < 200; i++) {
        uint32_t n = 0;
        IOReturn r = ctrl(0xc0, 0x01, 0x30, 0, ev, 64, &n);
        if (r == kIOReturnSuccess && n) { dropped++; empty = 0; continue; }
        empty++;
        IODelay(500);
    }
    if (dropped) say("dropped %u stale wmt events", dropped);
}

DlResult MtkBtFw::download()
{
    const uint8_t *fw = mtk_bt_fw;
    size_t fwlen = (size_t)(mtk_bt_fw_end - mtk_bt_fw);
    if (fwlen < FW_HDR_SIZE + FW_GD_SIZE) { say("firmware blob too small"); return DL_FAILED; }
    uint32_t nsec = le32(fw + FW_HDR_SIZE + 12);
    if (nsec > 16 || FW_HDR_SIZE + FW_GD_SIZE + (size_t)FW_SEC_MAP_SIZE * nsec > fwlen) { say("bad section count"); return DL_FAILED; }
    wmtAnswered = false;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *map = fw + FW_HDR_SIZE + FW_GD_SIZE + FW_SEC_MAP_SIZE * i;
        uint32_t off = le32(map + 4), dl = le32(map + 12 + 4);    // secoffset, bin_info_spec.dlsize
        if (!dl) continue;
        if ((size_t)off + dl > fwlen) { say("section %u out of range", i); return DL_FAILED; }
        uint8_t q[1 + SEC_SEND_SIZE];
        q[0] = 0;                                                  // legacy download mode
        memcpy(q + 1, map + SEC_COMMON_SIZE, SEC_SEND_SIZE);
        uint8_t f = 0;
        int tries = 20;
        for (; tries > 0; tries--) {
            int w = wmt(WMT_PATCH_DWNLD, 0, q, sizeof(q), &f);
            if (w == WMT_NO_EVENT && !wmtAnswered) { say("section %u: no answer to the first query", i); return DL_NO_FIRST_ANSWER; }
            if (w != WMT_OK) { say("section %u: query failed", i); return DL_FAILED; }
            if (f == 1) { IOSleep(100); continue; }                // in progress
            break;
        }
        if (!tries) { say("section %u: chip stays busy", i); return DL_FAILED; }
        if (f == 2) { say("section %u: already loaded", i); continue; }
        if (f != 0) { say("section %u: status %u", i, f); return DL_FAILED; }
        const uint8_t *p = fw + off;
        uint32_t left = dl, blocks = 0;
        bool first = true;
        while (left) {
            uint16_t n = (uint16_t)(left > 250 ? 250 : left);
            uint8_t flag = first ? 1 : (left - n == 0 ? 3 : 2);
            first = false;
            if (wmt(WMT_PATCH_DWNLD, flag, p, n, &f) != WMT_OK) { say("section %u: block %u failed", i, blocks); return DL_FAILED; }
            if (f == 1) { say("section %u: busy during download", i); return DL_FAILED; }
            p += n;
            left -= n;
            blocks++;
        }
        say("section %u: %u bytes in %u blocks", i, dl, blocks);
    }
    IOSleep(110);                                                  // firmware activation
    return DL_OK;
}

// btmtk_usb_subsys_reset, dev_id 0x7925 branch: CONNV3 reset register bit 5, then bits 8-15 cleared and bit 13 set, the endpoint
// reset option, bit 0; clear both UDMA interrupt status registers; 100 ms; wait for MTK_BT_MISC RST_DONE (polled every 20 ms,
// 1 s at most); the device id must read back non-zero.
bool MtkBtFw::subsysReset()
{
    uint32_t v = 0;
    if (!uhwRead(REG_RESET_CONNV3, &v) || !uhwWrite(REG_RESET_CONNV3, v | (1u << 5))) return false;
    if (!uhwRead(REG_RESET_CONNV3, &v)) return false;
    v = (v & 0xffff00ffu) | (1u << 13);
    if (!uhwWrite(REG_RESET_CONNV3, v)) return false;
    if (!uhwWrite(REG_EP_RST_OPT, EP_RST_IN_OUT_OPT)) return false;
    if (!uhwRead(REG_RESET_CONNV3, &v) || !uhwWrite(REG_RESET_CONNV3, v | 1u)) return false;
    if (!uhwWrite(REG_UDMA_INT_STA_BT, 0xff) || !uhwRead(REG_UDMA_INT_STA_BT, &v)) return false;
    if (!uhwWrite(REG_UDMA_INT_STA_BT1, 0xff) || !uhwRead(REG_UDMA_INT_STA_BT1, &v)) return false;
    IOSleep(100);
    bool done = false;
    uint64_t t0 = nowMs();
    for (;;) {
        if (uhwRead(REG_BT_MISC, &v) && (v & BT_RST_DONE)) { done = true; break; }
        if (nowMs() - t0 >= 1000) break;
        IOSleep(20);
    }
    if (!done) say("subsys reset: RST_DONE not set (misc %08x)", v);
    uint32_t id = 0;
    if (!regRead(REG_DEV_ID, &id) || !id) { say("subsys reset: no device id"); return false; }
    say("subsys reset done in %llu ms (misc %08x, id %04x)", nowMs() - t0 + 100, v, id & 0xffff);
    return done;
}

// HCI Read Local Version Information (0x1001) on the normal HCI path: command on the control endpoint, Command Complete on the
// interrupt endpoint. Answered only when the RAM code is running.
bool MtkBtFw::hciAlive()
{
    if (!intrPipe) { say("hci probe: no interrupt pipe"); return false; }
    hciGot = false;
    hciWait = true;
    cmd[0] = 0x01; cmd[1] = 0x10; cmd[2] = 0;
    IOReturn r = ctrl(0x20, 0x00, 0, 0, cmd, 3, nullptr);
    if (r != kIOReturnSuccess) { hciWait = false; say("hci probe: send failed %08x", r); return false; }
    for (int i = 0; i < 200 && !hciGot; i++) IOSleep(5);
    hciWait = false;
    if (!hciGot) { say("hci probe: no answer"); return false; }
    // 0e len ncmd 01 10 status hci_ver rev_lo rev_hi lmp_ver manuf_lo manuf_hi
    say("hci probe: status %u hci %u lmp %u manufacturer %04x", hciEv[5], hciEv[6], hciEv[9], hciEv[10] | hciEv[11] << 8);
    return hciEv[5] == 0;
}

void MtkBtFw::intrDone(void *owner, void *, IOReturn status, uint32_t bytes)
{
    MtkBtFw *me = (MtkBtFw *)owner;
    me->intrBusy = false;
    if (status == kIOReturnSuccess && bytes) {
        const uint8_t *p = (const uint8_t *)me->intrBuf->getBytesNoCopy();
        if (me->intrPackets == 0 && p) {
            uint32_t n = bytes < sizeof(me->intrFirst) ? bytes : (uint32_t)sizeof(me->intrFirst);
            memcpy(me->intrFirst, p, n);
            me->intrFirstLen = n;
        }
        if (me->hciWait && p && bytes >= 6 && p[0] == 0x0e && p[3] == 0x01 && p[4] == 0x10) {
            uint32_t n = bytes < sizeof(me->hciEv) ? bytes : (uint32_t)sizeof(me->hciEv);
            memset(me->hciEv, 0, sizeof(me->hciEv));
            memcpy(me->hciEv, p, n);
            me->hciGot = true;
        } else {
            me->intrPackets++;
        }
    }
    if (!me->intrStop && status != kIOReturnAborted && status != kIOReturnNotOpen) me->intrArm();
}

// HCI events the chip also queues on the interrupt endpoint during setup are read and dropped (Linux has its interrupt URB
// running at that time too), so they cannot back up while we poll the control endpoint.
void MtkBtFw::intrArm()
{
    if (!intrPipe || !intrBuf || intrStop) return;
    IOUSBHostCompletion c = { this, intrDone, nullptr };
    intrBusy = true;
    if (intrPipe->io(intrBuf, (uint32_t)intrBuf->getLength(), &c, 0) != kIOReturnSuccess) intrBusy = false;
}

void MtkBtFw::worker(void *arg)
{
    MtkBtFw *me = (MtkBtFw *)arg;
    me->run();
    me->release();
}

void MtkBtFw::run()
{
    uint64_t t0 = nowMs();
    state("loading");
    if (!intf->open(this)) { say("interface busy (open failed)"); state("error"); return; }

    // the interrupt IN endpoint of this interface, from the raw configuration descriptor (bLength / bDescriptorType walk)
    const uint8_t *cfg = (const uint8_t *)intf->getConfigurationDescriptor();
    const uint8_t *idesc = (const uint8_t *)intf->getInterfaceDescriptor();
    if (cfg && idesc) {
        uint32_t total = (uint32_t)cfg[2] | (uint32_t)cfg[3] << 8, off = 0;
        bool mine = false;
        while (off + 2 <= total && cfg[off] >= 2 && off + cfg[off] <= total) {
            const uint8_t *d = cfg + off;
            if (d[1] == 4 && d[0] >= 9) mine = d[2] == idesc[2] && d[3] == idesc[3];      // interface: number / alternate
            else if (d[1] == 5 && d[0] >= 7 && mine && (d[3] & 3) == 3 && (d[2] & 0x80)) {  // endpoint: interrupt, IN
                intrPipe = intf->copyPipe(d[2]);
                break;
            }
            off += d[0];
        }
    }
    if (intrPipe) {
        intrBuf = IOBufferMemoryDescriptor::withCapacity(1024, kIODirectionIn);
        intrArm();
    }

    {
        const uint8_t *fw = mtk_bt_fw;
        size_t fwlen = (size_t)(mtk_bt_fw_end - mtk_bt_fw);
        char date[17] = {0};
        if (fwlen >= FW_HDR_SIZE + FW_GD_SIZE) memcpy(date, fw, 16);
        say("firmware %s, %u sections, %lu bytes", date, fwlen >= FW_HDR_SIZE + FW_GD_SIZE ? le32(fw + FW_HDR_SIZE + 12) : 0,
            (unsigned long)fwlen);
    }

    bool ok = false, alreadyRunning = false;
    uint32_t attempt = 0;
    for (attempt = 1; attempt <= MAX_ATTEMPTS && !intrStop; attempt++) {
        uint64_t ta = nowMs();
        setProperty("bt-attempts", attempt, 32);
        if (attempt > 1) {
            say("attempt %u: subsystem reset", attempt);
            if (!subsysReset()) say("attempt %u: subsystem reset incomplete, downloading anyway", attempt);
            drainWmt();
        }
        uint32_t devId = 0, fwVer = 0;
        if (!regRead(REG_DEV_ID, &devId) || !regRead(REG_FW_VER, &fwVer)) { say("attempt %u: chip id read failed", attempt); continue; }
        say("attempt %u: chip %04x fw_ver %08x", attempt, devId & 0xffff, fwVer);
        if ((devId & 0xffff) != 0x7925) { say("not an MT7925, leaving it alone"); break; }
        DlResult d = download();
        if (d == DL_NO_FIRST_ANSWER && attempt == 1) {
            // RAM code from the previous boot still running and no longer in WMT mode? Then leave it as it is.
            if (hciAlive()) { say("firmware already running, controller answers HCI"); ok = alreadyRunning = true; break; }
        }
        if (d == DL_OK) {
            bool on = uhwWrite(REG_EP_RST_OPT, EP_RST_IN_OUT_OPT);
            uint8_t one = 1, f = 0;
            if (on && wmt(WMT_FUNC_CTRL, 0, &one, 1, &f) != WMT_OK) on = false;   // btmtk_usb_setup resets on a FUNC_CTRL timeout too
            if (on) { say("bluetooth function on (flag %u)", f); ok = true; break; }
        }
        say("attempt %u failed after %llu ms, %u interrupt packets so far", attempt, nowMs() - ta, intrPackets);
    }

    intrStop = true;
    if (intrPipe) {
        intrPipe->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, this);
        for (int i = 0; i < 100 && intrBusy; i++) IOSleep(10);
        intrPipe->release();
        intrPipe = nullptr;
    }
    intf->close(this);
    if (intrFirstLen) {
        char hex[3 * sizeof(intrFirst) + 1] = {0};
        for (uint32_t i = 0; i < intrFirstLen; i++) snprintf(hex + 3 * i, 4, "%02x ", intrFirst[i]);
        hex[3 * intrFirstLen - 1] = 0;
        say("first interrupt packet: %s", hex);
    }
    say("%s in %llu ms, %u interrupt packets dropped", ok ? (alreadyRunning ? "ready (already running)" : "ready") : "failed",
        nowMs() - t0, intrPackets);
    state(ok ? "ready" : "error");
}

bool MtkBtFw::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    intf = OSDynamicCast(IOUSBHostInterface, provider);
    if (!intf) return false;
    cmd = (uint8_t *)IOMalloc(512);
    ev = (uint8_t *)IOMalloc(64);
    if (!cmd || !ev) return false;
    state("starting");
    registerService();
    retain();                                                       // released by the worker
    startThread(worker, this);
    return true;
}

void MtkBtFw::stop(IOService *provider)
{
    intrStop = true;
    IOService::stop(provider);
}

void MtkBtFw::free()
{
    if (intrBuf) intrBuf->release();
    if (cmd) IOFree(cmd, 512);
    if (ev) IOFree(ev, 64);
    IOService::free();
}
