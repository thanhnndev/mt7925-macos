#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) 2026 thanhnndev and contributors
"""Check BtLeDlFix's bluetoothd patterns against the installed /usr/sbin/bluetoothd (x86_64 slice).

Reading the file goes through the page cache, which holds the pages BtLeDlFix patched while bluetoothd runs. So per patch:
  applied   - the patched bytes are there (bluetoothd is running with it)
  will-match - the original pattern is there exactly once (the patch applies when that page is validated, e.g. after a reboot
               or a bluetoothd restart)
  MISSING   - neither: after a macOS update the code changed; re-derive the pattern (docs/BLUETOOTH.md)
Run it after every macOS update, before relying on Bluetooth LE. Patterns and masks must match kexts/BtLeDlFix/BtLeDlFix.cpp.
"""
import re
import struct
import sys

# (name, macOS major it targets, find pattern with ?? wildcards, patched bytes at the start of the match, offset of the change)
PATCHES = [
    ("LE length wait (15)", "15", "83 f8 08 45 0f b6 f6 b8 00 00 00 00 44 0f 44 f0", "39 c0 90", 0),
    ("LE length wait (26)", "26", "83 f8 08 0f 95 c0 44 20 ?? 88 45 ??", "39 c0 90", 0),
    ("advertising sets check", "26", "48 8b 08 48 89 c7 ff 91 c0 09 00 00 41 89 c6", "b8 01 00 00 00 90", 6),
    ("random-address flag", "26", "83 b8 ?? ?? ?? ?? 03 77 ?? 31 ff e8 ?? ?? ?? ??", "0f 1f 44 00 00", 11),
    ("random-address resend", "26",
     "44 33 84 08 ?? ?? ?? ?? 8b 54 3a 03 33 94 08 ?? ?? ?? ?? 44 09 c2 0f 84 ?? ?? ?? ??", "66 0f 1f 44 00 00", 22),
]


def x86_slice(data):
    magic = struct.unpack(">I", data[:4])[0]
    if magic in (0xCAFEBABE, 0xCAFEBABF):
        n = struct.unpack(">I", data[4:8])[0]
        wide = magic == 0xCAFEBABF
        for i in range(n):
            if wide:
                cpu, _, off, size = struct.unpack(">iiQQ", data[8 + i * 32: 8 + i * 32 + 24])
            else:
                cpu, _, off, size = struct.unpack(">iiII", data[8 + i * 20: 8 + i * 20 + 16])
            if cpu == 0x01000007:
                return data[off: off + size]
        sys.exit("no x86_64 slice")
    return data


def rx(s):
    return re.compile(b"".join(b"." if t == "??" else re.escape(bytes([int(t, 16)])) for t in s.split()), re.S)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/usr/sbin/bluetoothd"
    d = x86_slice(open(path, "rb").read())
    bad = 0
    for name, major, find, patched, at in PATCHES:
        orig = [m.start() for m in rx(find).finditer(d)]
        toks = find.split()
        ptoks = patched.split()
        pfind = " ".join(toks[:at] + ptoks + toks[at + len(ptoks):])
        done = [m.start() for m in rx(pfind).finditer(d)]
        if len(done) == 1:
            state = "applied"
        elif len(orig) == 1:
            state = "will-match"
        elif len(orig) > 1:
            state = "AMBIGUOUS (%d matches)" % len(orig)
        else:
            state = "MISSING"
        if state not in ("applied", "will-match") and major == "26":
            bad += 1
        where = hex((done or orig or [0])[0])
        print("%-24s macOS %-3s %-22s %s" % (name, major, state, where if (done or orig) else ""))
    print("macOS 26 patches: %s" % ("all OK" if not bad else "%d need attention" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
