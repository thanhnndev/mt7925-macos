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
// A warm reboot does not power-cycle the chip. If the Bluetooth firmware of the previous session hung (its watchdog fired), the
// chip keeps that state: the control endpoint stalls, or WMT still answers but HCI does not and the firmware streams a coredump
// on the ACL pipe. bluetoothd then times out on HCI Reset, records the USB controller as failed and falls back to a UART
// controller that does not exist, restarting every 15 s. So after FUNC_CTRL the loader sends HCI Reset itself and waits for
// its Command Complete. If that, or any earlier step, fails, it resets the Bluetooth subsystem (Linux btmtk_usb_subsys_reset,
// MT7925 CONNV3 path; Linux reaches it through btmtk_reset_sync, see commit b3078bb2ee12 "mt7925: trigger reset on WMT
// timeout") and loads the whole firmware again, once. A dead firmware is also detected up front (WDT status, stalled register
// read) so the reset comes first.
//
// 0.3.0/0.3.1 held IOKit busy during the load so that bluetoothd's IOKitWaitQuiet waited for it. That worked, but the busy
// count propagates to the registry root, and WindowServer and loginwindow wait for root quiet too: the login window came
// up as late as the firmware load (boot 2026-11-23 14:33, 9.7 s with a reset). 0.4.0 removes the hold (and mtkbtbusy).
//
// 0.4.0, device-level load. The driver matches the IOUSBHostDevice (0489:e11e) instead of interface 0, so it is the
// device's driver in place of AppleUSBHostCompositeDevice. It opens the device (one non-interface session: bluetoothd's
// device user client cannot open it or set a configuration meanwhile), selects configuration 1 without registering the
// interfaces, loads the firmware through interface 0 and only then registers the interfaces (what the composite driver
// does in one step). bluetoothd sees either no usable controller yet or a loaded one, never a half-loaded one.
// The device has only the Bluetooth interfaces 0-2 (ACL, SCO, ISO); Wi-Fi is the PCIe function (itlmt7925). USB port
// mapping (USBToolBox, HS03) is at the controller and is not touched.
//
// bluetoothd picks USB or UART once at start. On this board-id (Mac-5F9802EFE386AA28, MacBookPro16,2) it picks UART and
// scans for USB dongles once, so a load still running at that moment cost the controller for the whole boot. Before the
// board-id table it asks IOKit for a service named "Bluetooth USB Host Controller" (Apple's own USB controllers) and picks
// USB if there is one (bluetoothd 25G332, the transport getter's dispatch_once block). This driver registers itself under
// that name, so bluetoothd takes the USB transport, whose device scan retries every second for 15 s. Off switch:
// boot-arg mtkbtname=0 (the driver keeps its class name and bluetoothd picks by board-id again).
//
// Warm restart: the chip keeps its firmware. If WMT does not answer the very first PATCH_DWNLD query (no event, nothing
// written yet) while the register reads and the watchdog status look healthy, the running firmware is probed over HCI:
// HCI Reset must complete with status 0 and Read Local Version must complete with status 0 and manufacturer 0x0046
// (MediaTek). Only then is the download skipped. Anything else (no event, other status, wrong manufacturer) takes the
// subsystem reset and full reload as before. A cold chip answers WMT, so it is never probed.
//
// IOLog of these kexts does not reach dmesg or the unified log on this machine: the result is in the registry,
//   ioreg -rc MtkBtFw -l | grep -E '"bt-state"|"bt-log"'
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
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
#define REG_DEV_ID        0x70010200
#define REG_FW_VER        0x80021004
#define REG_EP_RST_OPT    0x74011890
#define EP_RST_IN_OUT_OPT 0x00010001
#define REG_BT_MISC       0x70002510
#define BT_RST_DONE       0x00000100
#define REG_RESET_CONNV3  0x70028610
#define REG_WDT_STATUS    0x740003A0
#define REG_UDMA_INT_STA  0x74000024
#define REG_UDMA_INT_STA1 0x74000308
#define MTK_CONFIG        1
#define MTK_USB_NAME      "Bluetooth USB Host Controller"   // bluetoothd: IOServiceNameMatching -> USB transport

static bool startThread(void (*fn)(void *), void *arg)
{
    thread_t th = nullptr;
    if (kernel_thread_start((thread_continue_t)fn, arg, &th) != KERN_SUCCESS) return false;
    thread_deallocate(th);
    return true;
}

static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

class MtkBtFw : public IOService {
    OSDeclareDefaultStructors(MtkBtFw)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool willTerminate(IOService *provider, IOOptionBits options) override;
    void free() override;
private:
    IOUSBHostDevice *dev = nullptr;
    IOUSBHostInterface *intf = nullptr;                // interface 0, open only while the firmware loads
    volatile bool gone = false;                         // the device is being terminated: stop talking to it
    IOUSBHostPipe *intrPipe = nullptr;
    IOBufferMemoryDescriptor *intrBuf = nullptr;
    volatile bool intrStop = false, intrBusy = false;
    uint32_t intrPackets = 0;
    volatile uint32_t ccSeq = 0;                     // bumped by intrDone for every HCI Command Complete
    volatile uint16_t ccOp = 0;                      // opcode of the last Command Complete
    uint8_t ccData[16] = {0};                        // its return parameters (status first)
    volatile uint8_t ccLen = 0;
    bool wmtSilent = false;                          // the first PATCH_DWNLD query got no event and nothing was written
    uint8_t *cmd = nullptr, *ev = nullptr;          // control transfer buffers (wired)
    char log[1200] = {0};
    size_t loglen = 0;

    static void worker(void *arg);
    void run();
    bool load();
    IOUSBHostInterface *findInterface0();
    void registerInterfaces();
    void say(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void state(const char *s) { setProperty("bt-state", s); }
    IOReturn ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, uint16_t len, uint32_t *done);
    bool regRead(uint32_t reg, uint32_t *val);
    bool uhwWrite(uint32_t reg, uint32_t val);
    bool uhwRead(uint32_t reg, uint32_t *val);
    bool subsysReset();
    bool hciCmd(uint16_t op, uint8_t *ret, uint8_t *retLen);
    bool hciReset();
    bool probeRunning();
    bool bringUp();
    int wmt(uint8_t op, uint8_t flag, const uint8_t *data, uint16_t dlen, uint8_t *evFlag);
    bool download();
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

// btmtk_usb_uhw_reg_write: request type 0x5e, bRequest 0x02
bool MtkBtFw::uhwWrite(uint32_t reg, uint32_t val)
{
    cmd[0] = (uint8_t)val; cmd[1] = (uint8_t)(val >> 8); cmd[2] = (uint8_t)(val >> 16); cmd[3] = (uint8_t)(val >> 24);
    IOReturn r = ctrl(0x5e, 0x02, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xffff), cmd, 4, nullptr);
    if (r != kIOReturnSuccess) { say("uhw %08x write failed %08x", reg, r); return false; }
    return true;
}

// btmtk_usb_uhw_reg_read: request type 0xde, bRequest 0x01. Answered by the USB hardware, also when the firmware is dead.
bool MtkBtFw::uhwRead(uint32_t reg, uint32_t *val)
{
    uint32_t n = 0;
    IOReturn r = ctrl(0xde, 0x01, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xffff), ev, 4, &n);
    if (r != kIOReturnSuccess || n != 4) { say("uhw %08x read failed %08x n=%u", reg, r, n); return false; }
    *val = le32(ev);
    return true;
}

// btmtk_usb_subsys_reset for dev_id 0x7925 (CONNV3), then the RST_DONE poll and the device id check.
bool MtkBtFw::subsysReset()
{
    uint32_t v = 0;
    if (!uhwRead(REG_RESET_CONNV3, &v) || !uhwWrite(REG_RESET_CONNV3, v | (1u << 5))) return false;
    if (!uhwRead(REG_RESET_CONNV3, &v)) return false;
    v = (v & 0xFFFF00FFu) | (1u << 13);
    if (!uhwWrite(REG_RESET_CONNV3, v) || !uhwWrite(REG_EP_RST_OPT, EP_RST_IN_OUT_OPT)) return false;
    if (!uhwRead(REG_RESET_CONNV3, &v) || !uhwWrite(REG_RESET_CONNV3, v | 1u)) return false;
    if (!uhwWrite(REG_UDMA_INT_STA, 0xff) || !uhwRead(REG_UDMA_INT_STA, &v)) return false;
    if (!uhwWrite(REG_UDMA_INT_STA1, 0xff) || !uhwRead(REG_UDMA_INT_STA1, &v)) return false;
    IOSleep(100);
    bool done = false;
    for (int i = 0; i < 50 && !done && !gone; i++) {                        // readx_poll_timeout(20 ms, 1 s)
        if (uhwRead(REG_BT_MISC, &v) && (v & BT_RST_DONE)) done = true;
        else IOSleep(20);
    }
    uint32_t id = 0;
    bool idOk = regRead(REG_DEV_ID, &id) && id;
    say("subsys reset: %s, dev id %08x", done ? "done" : "timeout", id);
    return done && idOk;
}

// One HCI command (no parameters) on the control endpoint; its Command Complete arrives on the interrupt endpoint (intrDone).
// Returns false when no Command Complete for this opcode came within 2 s.
bool MtkBtFw::hciCmd(uint16_t op, uint8_t *ret, uint8_t *retLen)
{
    uint32_t seq = ccSeq;
    cmd[0] = (uint8_t)op; cmd[1] = (uint8_t)(op >> 8); cmd[2] = 0x00;
    IOReturn r = ctrl(0x20, 0x00, 0, 0, cmd, 3, nullptr);
    if (r != kIOReturnSuccess) { say("HCI %04x send failed %08x", op, r); return false; }
    for (int i = 0; i < 200 && !gone; i++) {
        if (ccSeq != seq) {
            seq = ccSeq;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (ccOp == op) {
                uint8_t n = ccLen;
                if (ret) memcpy(ret, ccData, n);
                if (retLen) *retLen = n;
                return true;
            }
        }
        IOSleep(10);
    }
    say("HCI %04x: no Command Complete in 2 s", op);
    return false;
}

// HCI Reset as bluetoothd will send it.
bool MtkBtFw::hciReset()
{
    if (!intrPipe) { say("no interrupt pipe, HCI check skipped"); return true; }
    uint8_t rp[16], n = 0;
    if (!hciCmd(0x0c03, rp, &n) || n < 1) return false;
    say("HCI Reset complete, status %u", rp[0]);
    return rp[0] == 0;
}

// Strict check that the firmware left running by the previous session works: HCI Reset status 0, then Read Local Version
// status 0 from a MediaTek controller (manufacturer 0x0046). Needs the interrupt pipe to see the answers.
bool MtkBtFw::probeRunning()
{
    if (!intrPipe) { say("probe: no interrupt pipe"); return false; }
    uint8_t rp[16], n = 0;
    if (!hciCmd(0x0c03, rp, &n) || n < 1 || rp[0] != 0) { say("probe: HCI Reset failed (n=%u status %u)", n, n ? rp[0] : 0xff); return false; }
    if (!hciCmd(0x1001, rp, &n) || n < 9 || rp[0] != 0) { say("probe: Read Local Version failed (n=%u)", n); return false; }
    uint16_t manuf = (uint16_t)(rp[5] | rp[6] << 8), sub = (uint16_t)(rp[7] | rp[8] << 8);
    say("probe: HCI %u rev %04x LMP %u manufacturer %04x subversion %04x", rp[1], (unsigned)(rp[2] | rp[3] << 8), rp[4], manuf, sub);
    return manuf == 0x0046;
}

// Download, endpoint reset option, FUNC_CTRL 1, HCI check.
bool MtkBtFw::bringUp()
{
    if (!download() || !uhwWrite(REG_EP_RST_OPT, EP_RST_IN_OUT_OPT)) return false;
    uint8_t on = 1, f = 0;
    if (wmt(WMT_FUNC_CTRL, 0, &on, 1, &f) < 0) return false;
    say("bluetooth function on (flag %u)", f);
    return hciReset();
}

// One WMT command and its event. Returns 0 and the event's flag byte, or -1.
int MtkBtFw::wmt(uint8_t op, uint8_t flag, const uint8_t *data, uint16_t dlen, uint8_t *evFlag)
{
    uint32_t hlen = 5u + dlen;
    if (hlen > 255) return -1;
    cmd[0] = 0x6f; cmd[1] = 0xfc; cmd[2] = (uint8_t)hlen;           // HCI command 0xfc6f
    cmd[3] = 1; cmd[4] = op; cmd[5] = (uint8_t)(dlen + 1); cmd[6] = (uint8_t)((dlen + 1) >> 8); cmd[7] = flag;
    if (dlen) memcpy(cmd + 8, data, dlen);
    IOReturn r = ctrl(0x20, 0x00, 0, 0, cmd, (uint16_t)(3 + hlen), nullptr);
    if (r != kIOReturnSuccess) { say("wmt op %u send failed %08x", op, r); return -1; }
    for (int i = 0; i < 4000 && !gone; i++) {                       // ~2 s at 0.5 ms per empty poll
        uint32_t n = 0;
        r = ctrl(0xc0, 0x01, 0x30, 0, ev, 64, &n);
        if (r == kIOReturnSuccess && n >= 7) {
            if (ev[0] != 0xe4 || ev[3] != op) { say("wmt op %u: other event %02x op %u n=%u", op, ev[0], ev[3], n); continue; }
            if (evFlag) *evFlag = ev[6];
            return 0;
        }
        IODelay(500);
    }
    say("wmt op %u flag %u: no event", op, flag);
    return -1;
}

bool MtkBtFw::download()
{
    const uint8_t *fw = mtk_bt_fw;
    size_t fwlen = (size_t)(mtk_bt_fw_end - mtk_bt_fw);
    if (fwlen < FW_HDR_SIZE + FW_GD_SIZE) { say("firmware blob too small"); return false; }
    uint32_t nsec = le32(fw + FW_HDR_SIZE + 12);
    char date[17] = {0};
    memcpy(date, fw, 16);
    say("firmware %s, %u sections, %lu bytes", date, nsec, (unsigned long)fwlen);
    if (nsec > 16 || FW_HDR_SIZE + FW_GD_SIZE + (size_t)FW_SEC_MAP_SIZE * nsec > fwlen) { say("bad section count"); return false; }
    bool firstQuery = true;
    wmtSilent = false;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *map = fw + FW_HDR_SIZE + FW_GD_SIZE + FW_SEC_MAP_SIZE * i;
        uint32_t off = le32(map + 4), dl = le32(map + 12 + 4);    // secoffset, bin_info_spec.dlsize
        if (!dl) continue;
        if ((size_t)off + dl > fwlen) { say("section %u out of range", i); return false; }
        uint8_t q[1 + SEC_SEND_SIZE];
        q[0] = 0;                                                  // legacy download mode
        memcpy(q + 1, map + SEC_COMMON_SIZE, SEC_SEND_SIZE);
        uint8_t f = 0;
        int tries = 20;
        for (; tries > 0; tries--) {
            if (wmt(WMT_PATCH_DWNLD, 0, q, sizeof(q), &f) < 0) {
                if (firstQuery) wmtSilent = true;                  // nothing written yet: the running firmware may answer HCI
                return false;
            }
            firstQuery = false;
            if (f == 1) { IOSleep(100); continue; }                // in progress
            break;
        }
        if (!tries) { say("section %u: chip stays busy", i); return false; }
        if (f == 2) { say("section %u: already loaded", i); continue; }
        if (f != 0) { say("section %u: status %u", i, f); return false; }
        const uint8_t *p = fw + off;
        uint32_t left = dl, blocks = 0;
        bool first = true;
        while (left) {
            uint16_t n = (uint16_t)(left > 250 ? 250 : left);
            uint8_t flag = first ? 1 : (left - n == 0 ? 3 : 2);
            first = false;
            if (wmt(WMT_PATCH_DWNLD, flag, p, n, &f) < 0) { say("section %u: block %u failed", i, blocks); return false; }
            if (f == 1) { say("section %u: busy during download", i); return false; }
            p += n;
            left -= n;
            blocks++;
        }
        say("section %u: %u bytes in %u blocks", i, dl, blocks);
    }
    IOSleep(110);                                                  // firmware activation
    return true;
}

void MtkBtFw::intrDone(void *owner, void *, IOReturn status, uint32_t bytes)
{
    MtkBtFw *me = (MtkBtFw *)owner;
    me->intrBusy = false;
    if (status == kIOReturnSuccess) {
        me->intrPackets++;
        const uint8_t *e = (const uint8_t *)me->intrBuf->getBytesNoCopy();
        if (bytes >= 6 && e[0] == 0x0e && e[1] >= 4) {                 // Command Complete: len, ncmd, opcode, return params
            uint32_t n = bytes - 5;
            if (n > e[1] - 3u) n = e[1] - 3u;
            if (n > sizeof(me->ccData)) n = sizeof(me->ccData);
            memcpy(me->ccData, e + 5, n);
            me->ccLen = (uint8_t)n;
            me->ccOp = (uint16_t)(e[3] | e[4] << 8);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            me->ccSeq++;
        }
    }
    if (!me->intrStop && !me->gone && status != kIOReturnAborted && status != kIOReturnNotOpen) me->intrArm();
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

// The child interface 0 of the configured device (created by setConfiguration, registered or not).
IOUSBHostInterface *MtkBtFw::findInterface0()
{
    IOUSBHostInterface *found = nullptr;
    OSIterator *it = dev->getChildIterator(gIOServicePlane);
    if (!it) return nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOUSBHostInterface *i = OSDynamicCast(IOUSBHostInterface, o);
        const StandardUSB::InterfaceDescriptor *d = i ? i->getInterfaceDescriptor() : nullptr;
        if (d && d->bInterfaceNumber == 0) { found = i; found->retain(); break; }
    }
    it->release();
    return found;
}

// What AppleUSBHostCompositeDevice does right after setConfiguration(cfg, true): let the interfaces match.
void MtkBtFw::registerInterfaces()
{
    OSIterator *it = dev->getChildIterator(gIOServicePlane);
    if (!it) return;
    uint32_t n = 0;
    while (OSObject *o = it->getNextObject()) {
        IOUSBHostInterface *i = OSDynamicCast(IOUSBHostInterface, o);
        if (i && !i->isInactive() && !(i->getState() & kIOServiceRegisteredState)) { i->registerService(); n++; }
    }
    it->release();
    say("%u interfaces registered", n);
}

void MtkBtFw::run()
{
    uint64_t t0 = 0, t1 = 0;
    clock_get_uptime(&t0);
    state("loading");
    // One session on the device for the whole load: bluetoothd's device user client cannot open it (and cannot select a
    // configuration, which would tear down the interface we load through) until we close it.
    if (!dev->open(this)) { say("device busy (open failed), leaving it alone"); state("error"); return; }
    bool ok = false;
    bool configured = dev->getConfigurationDescriptor() != nullptr;
    if (!configured) {
        IOReturn r = dev->setConfiguration(MTK_CONFIG, false);    // interfaces created, not registered for matching
        if (r != kIOReturnSuccess) say("set configuration %u failed %08x", MTK_CONFIG, r);
        else configured = true;
    } else say("device already configured");
    if (configured && !gone) {
        intf = findInterface0();
        if (!intf) say("no interface 0");
        else if (!intf->open(this)) say("interface 0 busy (open failed)");
        else {
            ok = load();
            intf->close(this);
        }
        if (intf) { intf->release(); intf = nullptr; }
        if (!gone) registerInterfaces();                            // also after a failed load: bluetoothd decides then
    }
    dev->close(this);
    clock_get_uptime(&t1);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    say("%s in %llu ms, %u interrupt packets dropped", gone ? "device gone" : ok ? "ready" : "failed", ns / 1000000ull, intrPackets);
    state(gone ? "gone" : ok ? "ready" : "error");
}

// Firmware load through the open interface 0. Returns true when the controller answers HCI.
bool MtkBtFw::load()
{

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

    // A stalled register read or a set watchdog status means the firmware of the previous session hung: reset first.
    uint32_t devId = 0, fwVer = 0, wdt = 0;
    bool ok = regRead(REG_DEV_ID, &devId);
    bool fwAlive = ok && regRead(REG_FW_VER, &fwVer);
    if (uhwRead(REG_WDT_STATUS, &wdt) && wdt) fwAlive = false;
    if (!ok) { ok = subsysReset() && regRead(REG_DEV_ID, &devId); fwAlive = ok; }
    if (ok) say("chip %04x fw_ver %08x wdt %u", devId & 0xffff, fwVer, wdt);
    if (ok && (devId & 0xffff) != 0x7925) { say("not an MT7925, leaving it alone"); ok = false; }
    else if (ok) {
        if (!fwAlive) { say("firmware of the previous session is dead, resetting"); ok = subsysReset(); }
        if (ok && !bringUp()) {
            if (fwAlive && wmtSilent && !gone && probeRunning()) say("WMT silent, running firmware answers HCI: download skipped");
            else if (gone) ok = false;
            else {
                say("bring-up failed, subsystem reset and full reload");
                ok = subsysReset() && bringUp();
            }
        }
    }
    intrStop = true;
    if (intrPipe) {
        intrPipe->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, this);
        for (int i = 0; i < 100 && intrBusy; i++) IOSleep(10);
        intrPipe->release();
        intrPipe = nullptr;
    }
    return ok && !gone;
}

bool MtkBtFw::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    dev = OSDynamicCast(IOUSBHostDevice, provider);
    if (!dev) return false;
    cmd = (uint8_t *)IOMalloc(512);
    ev = (uint8_t *)IOMalloc(64);
    if (!cmd || !ev) return false;
    state("starting");
    uint32_t nameArg = 1;
    if (PE_parse_boot_argn("mtkbtname", &nameArg, sizeof nameArg) && nameArg == 0) setProperty("bt-transport", "board-id (mtkbtname=0)");
    else {
        setName(MTK_USB_NAME);                                      // before registerService: bluetoothd looks it up by name
        setProperty("bt-transport", "usb (named " MTK_USB_NAME ")");
    }
    registerService();
    retain();                                                       // released by the worker
    if (!startThread(worker, this)) {
        say("could not start the loader thread");
        state("error");
        if (!dev->getConfigurationDescriptor() && dev->open(this)) {   // still hand the interfaces to bluetoothd
            if (dev->setConfiguration(MTK_CONFIG, true) != kIOReturnSuccess) say("set configuration failed");
            dev->close(this);
        }
        release();
    }
    return true;
}

// Unplug, or the device re-enumerates: everything in flight fails from here on; the worker sees `gone`, stops early and
// closes the interface and the device, which lets the termination finish.
bool MtkBtFw::willTerminate(IOService *provider, IOOptionBits options)
{
    gone = true;
    intrStop = true;
    return IOService::willTerminate(provider, options);
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
