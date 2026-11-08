// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// mtkbtctl: user-space check and recovery for the MT7925 Bluetooth function (USB 0489:e11e, interface 0), the same
// procedure as kexts/MtkBtFw, for use while the system is running.
//
//   mtkbtctl regs               UHW registers (read only)
//   mtkbtctl probe              HCI Reset -> is the controller answering? Also drains the bulk IN pipe and counts
//                               firmware coredump / log packets (ACL handles 0xfc6f, 0x05ff, 0x05fe, 0x2efd).
//   mtkbtctl recover <fw.bin>   Linux btmtk_usb_subsys_reset (MT7925 CONNV3 path), full firmware download, endpoint reset
//                               option, WMT FUNC_CTRL 1, then HCI Reset to confirm.
//
// bluetoothd must not have the interface open (it does not while it sits on the UART fallback).
// Build: clang -O2 -Wall -framework IOKit -framework CoreFoundation -o mtkbtctl tools/mtkbtctl.c
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef IOUSBInterfaceInterface300 **Intf;
static Intf gi;
static UInt8 intrPipe, bulkInPipe;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static IOReturn ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, uint16_t len, uint32_t *done)
{
    IOUSBDevRequestTO r = {0};
    r.bmRequestType = type; r.bRequest = req; r.wValue = value; r.wIndex = index; r.wLength = len; r.pData = buf;
    r.noDataTimeout = 1000; r.completionTimeout = 1000;
    IOReturn ret = (*gi)->ControlRequestTO(gi, 0, &r);
    if (done) *done = r.wLenDone;
    return ret;
}
static int regRead(uint32_t reg, uint32_t *v)          // btmtk_usb_reg_read: vendor IN 0x63
{
    uint8_t b[4]; uint32_t n = 0;
    IOReturn r = ctrl(0xc0, 0x63, reg >> 16, reg & 0xffff, b, 4, &n);
    if (r || n != 4) { printf("reg %08x read failed %08x\n", reg, r); return -1; }
    *v = le32(b); return 0;
}
static int uhwRead(uint32_t reg, uint32_t *v)          // btmtk_usb_uhw_reg_read: type 0xde, bRequest 1
{
    uint8_t b[4]; uint32_t n = 0;
    IOReturn r = ctrl(0xde, 0x01, reg >> 16, reg & 0xffff, b, 4, &n);
    if (r || n != 4) { printf("uhw %08x read failed %08x\n", reg, r); return -1; }
    *v = le32(b); return 0;
}
static int uhwWrite(uint32_t reg, uint32_t v)          // btmtk_usb_uhw_reg_write: type 0x5e, bRequest 2
{
    uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 };
    IOReturn r = ctrl(0x5e, 0x02, reg >> 16, reg & 0xffff, b, 4, NULL);
    if (r) { printf("uhw %08x write failed %08x\n", reg, r); return -1; }
    return 0;
}
static int wmt(uint8_t op, uint8_t flag, const uint8_t *data, uint16_t dlen, uint8_t *evFlag)
{
    uint8_t cmd[300], ev[64];
    uint32_t hlen = 5u + dlen;
    cmd[0] = 0x6f; cmd[1] = 0xfc; cmd[2] = hlen; cmd[3] = 1; cmd[4] = op; cmd[5] = dlen + 1; cmd[6] = (dlen + 1) >> 8; cmd[7] = flag;
    if (dlen) memcpy(cmd + 8, data, dlen);
    IOReturn r = ctrl(0x20, 0x00, 0, 0, cmd, 3 + hlen, NULL);
    if (r) { printf("wmt op %u send failed %08x\n", op, r); return -1; }
    for (int i = 0; i < 2000; i++) {
        uint32_t n = 0;
        r = ctrl(0xc0, 0x01, 0x30, 0, ev, sizeof(ev), &n);
        if (r == kIOReturnSuccess && n >= 7) {
            if (ev[0] != 0xe4 || ev[3] != op) continue;
            if (evFlag) *evFlag = ev[6];
            return 0;
        }
        usleep(1000);
    }
    printf("wmt op %u flag %u: no event\n", op, flag);
    return -1;
}

// Bulk IN: ACL data. Counts the MediaTek diagnostic handles that Linux btusb routes to the diag channel.
static void drainBulk(int ms)
{
    uint8_t b[1024];
    unsigned pk = 0, dump = 0, log = 0, other = 0;
    for (;;) {
        UInt32 n = sizeof(b);
        IOReturn r = (*gi)->ReadPipeTO(gi, bulkInPipe, b, &n, ms, ms);
        if (r != kIOReturnSuccess) { if (r != kIOUSBTransactionTimeout) printf("bulk read %08x\n", r); (*gi)->ClearPipeStallBothEnds(gi, bulkInPipe); break; }
        pk++;
        uint16_t h = n >= 2 ? (b[0] | b[1] << 8) : 0;
        if (h == 0xfc6f) dump++; else if (h == 0x05ff || h == 0x05fe || h == 0x2efd) log++; else other++;
        if (pk <= 3) { printf("acl %u bytes handle %04x:", (unsigned)n, h); for (UInt32 i = 0; i < n && i < 24; i++) printf(" %02x", b[i]); printf("\n"); }
        if (pk > 200000) break;
    }
    printf("bulk IN drained: %u packets (coredump 0xfc6f %u, fw log %u, other %u)\n", pk, dump, log, other);
}

// Interrupt IN read with a timeout (ReadPipeTO is bulk only): async read, run the run loop, abort on timeout.
static IOReturn gIntrRes; static UInt32 gIntrLen; static int gIntrDone;
static void intrCb(void *ref, IOReturn r, void *arg) { gIntrRes = r; gIntrLen = (UInt32)(uintptr_t)arg; gIntrDone = 1; }
static IOReturn readIntr(uint8_t *buf, UInt32 *len, int ms)
{
    static CFRunLoopSourceRef src;
    if (!src) { (*gi)->CreateInterfaceAsyncEventSource(gi, &src); CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode); }
    gIntrDone = 0;
    IOReturn r = (*gi)->ReadPipeAsync(gi, intrPipe, buf, *len, intrCb, NULL);
    if (r) return r;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, ms / 1000.0, false);
    while (!gIntrDone) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
        if (!gIntrDone) { (*gi)->AbortPipe(gi, intrPipe); CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, false); if (!gIntrDone) return kIOReturnTimeout; }
    }
    *len = gIntrLen;
    return gIntrRes;
}

// HCI_Reset on the control endpoint, Command Complete on the interrupt endpoint.
static int hciReset(void)
{
    uint8_t c[3] = { 0x03, 0x0c, 0x00 }, e[64];
    IOReturn r = ctrl(0x20, 0x00, 0, 0, c, 3, NULL);
    if (r) { printf("HCI Reset send failed %08x\n", r); return -1; }
    for (int i = 0; i < 20; i++) {
        UInt32 n = sizeof(e);
        r = readIntr(e, &n, 2000);
        if (r != kIOReturnSuccess) { printf("HCI Reset: no event (%08x)\n", r); return -1; }
        printf("event:"); for (UInt32 k = 0; k < n && k < 16; k++) printf(" %02x", e[k]); printf("\n");
        if (n >= 6 && e[0] == 0x0e && e[3] == 0x03 && e[4] == 0x0c) { printf("HCI Reset complete, status %02x\n", e[5]); return e[5] ? -1 : 0; }
    }
    return -1;
}

#define MTK_BT_MISC             0x70002510
#define MTK_BT_RESET_REG_CONNV3 0x70028610
#define MTK_EP_RST_OPT          0x74011890
#define MTK_UDMA_INT_STA_BT     0x74000024
#define MTK_UDMA_INT_STA_BT1    0x74000308

static int subsysReset(void)                          // btmtk_usb_subsys_reset, dev_id 0x7925
{
    uint32_t v;
    if (uhwRead(MTK_BT_RESET_REG_CONNV3, &v)) return -1;
    printf("CONNV3 reset reg %08x\n", v);
    if (uhwWrite(MTK_BT_RESET_REG_CONNV3, v | (1 << 5))) return -1;
    if (uhwRead(MTK_BT_RESET_REG_CONNV3, &v)) return -1;
    v &= 0xFFFF00FF; v |= (1 << 13);
    if (uhwWrite(MTK_BT_RESET_REG_CONNV3, v)) return -1;
    if (uhwWrite(MTK_EP_RST_OPT, 0x00010001)) return -1;
    if (uhwRead(MTK_BT_RESET_REG_CONNV3, &v)) return -1;
    if (uhwWrite(MTK_BT_RESET_REG_CONNV3, v | 1)) return -1;
    if (uhwWrite(MTK_UDMA_INT_STA_BT, 0xff) || uhwRead(MTK_UDMA_INT_STA_BT, &v)) return -1;
    if (uhwWrite(MTK_UDMA_INT_STA_BT1, 0xff) || uhwRead(MTK_UDMA_INT_STA_BT1, &v)) return -1;
    usleep(100000);
    int done = 0;
    for (int i = 0; i < 50 && !done; i++) {             // readx_poll_timeout 20 ms / 1 s
        if (uhwRead(MTK_BT_MISC, &v) == 0 && (v & 0x100)) done = 1; else usleep(20000);
    }
    printf("reset %s (MISC %08x)\n", done ? "done" : "TIMEOUT", v);
    uint32_t id = 0;
    if (regRead(0x70010200, &id) || !id) { printf("no device id after reset\n"); return -1; }
    printf("device id %08x\n", id);
    return done ? 0 : -1;
}

static int download(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    static uint8_t fw[1 << 20];
    size_t len = fread(fw, 1, sizeof(fw), f);
    fclose(f);
    uint32_t nsec = le32(fw + 32 + 12);
    printf("firmware %.16s, %u sections, %zu bytes\n", (char *)fw, nsec, len);
    if (nsec > 16) return -1;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *map = fw + 32 + 64 + 64 * i;
        uint32_t off = le32(map + 4), dl = le32(map + 16);
        if (!dl) continue;
        if ((size_t)off + dl > len) return -1;
        uint8_t q[53]; q[0] = 0; memcpy(q + 1, map + 12, 52);
        uint8_t st = 0; int tries = 20;
        for (; tries; tries--) { if (wmt(1, 0, q, sizeof(q), &st)) return -1; if (st == 1) { usleep(100000); continue; } break; }
        if (!tries) { printf("section %u busy\n", i); return -1; }
        if (st == 2) { printf("section %u: already loaded\n", i); continue; }
        if (st) { printf("section %u: status %u\n", i, st); return -1; }
        const uint8_t *p = fw + off; uint32_t left = dl, blocks = 0; int first = 1;
        while (left) {
            uint16_t n = left > 250 ? 250 : left;
            uint8_t fl = first ? 1 : (left - n == 0 ? 3 : 2); first = 0;
            if (wmt(1, fl, p, n, &st)) { printf("section %u block %u failed\n", i, blocks); return -1; }
            p += n; left -= n; blocks++;
        }
        printf("section %u: %u bytes in %u blocks\n", i, dl, blocks);
    }
    usleep(110000);
    return 0;
}

static int openIntf(void)
{
    io_iterator_t it = 0;
    io_service_t s = 0, e;
    IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostInterface"), &it);
    while (!s && (e = IOIteratorNext(it))) {
        int v = 0, p = 0, n = -1;
        CFTypeRef cv = IORegistryEntrySearchCFProperty(e, kIOServicePlane, CFSTR("idVendor"), NULL, kIORegistryIterateRecursively | kIORegistryIterateParents);
        CFTypeRef cp = IORegistryEntrySearchCFProperty(e, kIOServicePlane, CFSTR("idProduct"), NULL, kIORegistryIterateRecursively | kIORegistryIterateParents);
        CFTypeRef cn = IORegistryEntryCreateCFProperty(e, CFSTR("bInterfaceNumber"), NULL, 0);
        if (cv) { CFNumberGetValue(cv, kCFNumberIntType, &v); CFRelease(cv); }
        if (cp) { CFNumberGetValue(cp, kCFNumberIntType, &p); CFRelease(cp); }
        if (cn) { CFNumberGetValue(cn, kCFNumberIntType, &n); CFRelease(cn); }
        if (v == 0x0489 && p == 0xe11e && n == 0) s = e; else IOObjectRelease(e);
    }
    if (it) IOObjectRelease(it);
    if (!s) { printf("interface 0 of 0489:e11e not found\n"); return -1; }
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    IOReturn r = IOCreatePlugInInterfaceForService(s, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(s);
    if (r || !plug) { printf("plugin %08x\n", r); return -1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&gi);
    (*plug)->Release(plug);
    if (!gi) return -1;
    r = (*gi)->USBInterfaceOpen(gi);
    if (r) { printf("USBInterfaceOpen %08x (bluetoothd or another client has it)\n", r); return -1; }
    UInt8 np = 0; (*gi)->GetNumEndpoints(gi, &np);
    for (UInt8 p = 1; p <= np; p++) {
        UInt8 dir, num, tt, iv; UInt16 mps;
        (*gi)->GetPipeProperties(gi, p, &dir, &num, &tt, &mps, &iv);
        if (dir == kUSBIn && tt == kUSBInterrupt) intrPipe = p;
        if (dir == kUSBIn && tt == kUSBBulk) bulkInPipe = p;
    }
    printf("interface open, interrupt pipe %u, bulk IN pipe %u\n", intrPipe, bulkInPipe);
    return intrPipe && bulkInPipe ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s probe | recover <BT_RAM_CODE_MT7925_1_1_hdr.bin>\n", argv[0]); return 2; }
    if (openIntf()) return 1;
    int rc = 0;
    uint32_t id = 0, ver = 0;
    if (!regRead(0x70010200, &id) && !regRead(0x80021004, &ver)) printf("chip %04x fw_ver %08x\n", id & 0xffff, ver);
    if (!strcmp(argv[1], "regs")) {
        uint32_t v;
        if (!uhwRead(MTK_BT_MISC, &v)) printf("MISC %08x\n", v);
        if (!uhwRead(MTK_BT_RESET_REG_CONNV3, &v)) printf("CONNV3 reset %08x\n", v);
        if (!uhwRead(0x740003A0, &v)) printf("WDT status %08x\n", v);
        if (!uhwRead(MTK_EP_RST_OPT, &v)) printf("EP reset opt %08x\n", v);
    } else if (!strcmp(argv[1], "probe")) {
        drainBulk(300);
        rc = hciReset() ? 1 : 0;
        drainBulk(300);
    } else if (!strcmp(argv[1], "recover") && argc > 2) {
        drainBulk(300);
        if (subsysReset() || download(argv[2]) || uhwWrite(MTK_EP_RST_OPT, 0x00010001)) rc = 1;
        uint8_t on = 1, st = 0;
        if (!rc && wmt(6, 0, &on, 1, &st)) rc = 1;
        if (!rc) printf("bluetooth function on (flag %u)\n", st);
        if (!rc) { drainBulk(200); rc = hciReset() ? 1 : 0; }
    } else rc = 2;
    (*gi)->USBInterfaceClose(gi);
    (*gi)->Release(gi);
    printf("%s\n", rc ? "FAILED" : "OK");
    return rc;
}
