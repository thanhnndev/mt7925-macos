# bt-v1.2 (macOS 26 Tahoe): Bluetooth kexts, MtkBtFw 0.3.3

**Related releases:** [Hub v2 (macOS 26 Tahoe)](https://github.com/thanhnndev/ga403wm-hackintosh/releases/tag/v2) · Wi-Fi [v2.0-beta.1](https://github.com/thanhnndev/mt7925-macos/releases/tag/v2.0-beta.1) · [ROG Control 0.6.1](https://github.com/thanhnndev/rog-control-macos/releases/tag/rogcontrol-v0.6.1) · 890M driver source [ga403wm-890m-out50](https://github.com/thanhnndev/nvidia-macos-driver/releases/tag/ga403wm-890m-out50) · RyzenHV source [v0.7.24-ga403wm](https://github.com/thanhnndev/ryzen-hypervisor.framework/releases/tag/v0.7.24-ga403wm)

**Bluetooth kexts for the MediaTek MT7925 on macOS 26 Tahoe.** `MtkBtFw.kext` loads the Bluetooth firmware over USB;
`BtLeDlFix.kext` (a Lilu plugin) patches `bluetoothd` in memory so BLE devices connect and LE advertising starts. New in
this release: three more MT7925 USB ids, and **experimental, untested** MT7922 support. For macOS 15 use `bt-v1.0`.

> ## Warning: vibe-coded, one-laptop project
> Almost all of this code was written by an AI (Claude) and tested on **one** ASUS ROG Zephyrus G14 **GA403WM** on
> macOS 26.7.2 (`25G332`, Darwin 25.6). The kexts run in the macOS kernel and can panic or hang the machine.
> `BtLeDlFix` patches `bluetoothd` in memory. Nobody has audited any of it. No warranty. Back up first.

## What this release is

- Tag target: `fcc0baa` on `main`. `kexts/MtkBtFw` there is the same as at `d5c3f8b` (MtkBtFw 0.3.3).
- The zips are the files that ran on the test machine (boot of 2026-11-27 16:36), copied from its EFI with the debug
  symbol entries stripped (same UUIDs, same code and data). `BtLeDlFix.kext.zip` is the same file as in `bt-v1.1`.
- Wi-Fi is not part of this release; use `v2.0-beta.1`.

## What changed since `bt-v1.1` (2026-11-25)

Status words: **verified** = seen working on the GA403WM, with the date; **loaded** = runs daily, the specific effect
was not checked on its own; **not tested** = no test on any machine with this version.

| Item | Version | Status |
|---|---|---|
| MT7925 Bluetooth on the GA403WM (`0489:e11e`): firmware loads (v9736 c4881), Bluetooth on, 0 "STATUS 718" lines from `bluetoothd`, `bluetoothd` at 2.6 % CPU, no `bluetoothd` fault report since 2026-11-23 | MtkBtFw 0.3.3 + BtLeDlFix 1.4.0 | verified boot 2026-11-27 16:36 (MtkBtFw 0.3.3 loaded since 15:34). **BLE mouse: not tested on 0.3.3 yet** (last verified 2026-11-05 on an older MtkBtFw); this row is updated when it is |
| Matches the HP OEM MT7925 combo card (`0e8d:8c38`) and the Legion id (`0489:e111`) | MtkBtFw 0.3.2 | `0e8d:8c38`: by the reporter of [ga403wm-hackintosh#10](https://github.com/thanhnndev/ga403wm-hackintosh/issues/10) on macOS 15.8.1, with the ids changed by hand before 0.3.2. `0489:e111`: not tested |
| **MT7922 (experimental)**: matches `0489:e0f6` (GA403UV, RZ616), accepts chip id `7922`, loads linux-firmware's `BT_RAM_CODE_MT7922_1_1_hdr.bin` (byte-identical to linux-firmware `f997b2832a0f`). The MT7925 subsystem reset is never sent to an MT7922; a MT7922 firmware left running needs a cold boot. ROMs that ask for another firmware suffix than `_1_1` are left alone | MtkBtFw 0.3.3 | **not tested on 0.3.3** on any MT7922. Contributed by [hbyonly1](https://github.com/hbyonly1), who tested an earlier version on the older loader (GA403UV, macOS 15.8.1): a BLE mouse works, AirPods Pro 2 pair, AirPods playback starts in about 1 of 15 attempts |
| BlueToolFixup NVRAM values documented | docs | n/a |

For chip `7925` the code path is the same as in 0.3.1 / 0.3.2: same firmware bytes, same reset and bring-up.

## Known issues

- **The login-window delay of 4 to 9 s from 0.3.1** (WindowServer waits for the same registry quiet) is **not
  re-measured** on 0.3.3; the code is unchanged for chip `7925`. Off switch: boot-arg `mtkbtbusy=0`.
- `BtLeDlFix` matches byte patterns in `bluetoothd`. Run `tools/btledl-check.py` after every macOS update.
- MT7922: see the table. AirPods playback failed in most attempts on the contributor's machine; the cause is unknown.
- Keyboard pairing, and re-pairing after a restart: not recorded.
- The versions in this release were run on `25G332` only.
- AirDrop needs the AWDL work of the Wi-Fi driver, which is not usable yet.

## Install

[GUIDE §13](https://github.com/thanhnndev/mt7925-macos/blob/main/docs/GUIDE.md#13-bluetooth) ·
[BLUETOOTH.md](https://github.com/thanhnndev/mt7925-macos/blob/main/docs/BLUETOOTH.md).
Also needed, not included and not ours: [Lilu](https://github.com/acidanthera/Lilu) and `BlueToolFixup.kext` from
[BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases), plus two NVRAM values (see the guide).
Coming from `bt-v1.1`: replace `MtkBtFw.kext` only.

## Assets

Both kexts are the binaries that ran, with only the debug symbol entries removed (`strip -S`: they carried the build
paths of the machine that built them). The UUIDs are unchanged, every section of both binaries is byte-identical to the
EFI copies, and the `Info.plist` files are the ones that ran. Zipped with `ditto -c -k --keepParent`.

| File | Version | Source | UUID of the binary inside | SHA-256 |
|---|---|---|---|---|
| `MtkBtFw.kext.zip` | 0.3.3 | `d5c3f8b` | `41826D92-B6AB-344A-B54D-61B534358529` | `899699aa0824319d2d39e98bc19bbb6f397bd66cd31d9d456110caf243ba52fb` |
| `BtLeDlFix.kext.zip` | 1.4.0 | `0a23a6c` (same file as in `bt-v1.1`) | `C6274348-4A25-36D0-84D7-2E234AF13B8B` | `5853e63c6ae9530dbded67fc7426082ca300bf87d5dc782b2ca9afb9073eb956` |
| `LICENSE` | n/a | `fcc0baa` | n/a | `18ea8e404a91378ceb54eaa1514626b83d2c25a0aa6f6da3dbd2d4264e869304` |
| `LICENCE.mediatek` | n/a | `fw/LICENCE.mediatek` | n/a | `a90d3f66704d85889945fec5525ea77622549da83aced1aac99828383f8f1805` |
| `LICENSE.Lilu.txt` | n/a | acidanthera/Lilu `LICENSE.txt` at `0249c3a` | n/a | `74c3ca48189b796b0d262866e67de8efba77a71516667ce68dc3a8f13c2a3c57` |
| `SHA256SUMS.txt` | n/a | the five files above | n/a | n/a |

SHA-256 of the `MtkBtFw` binary inside the zip: `45a793ac591d7fab3e8f96104c54f52808158dea173c559c27b36e1af99c70a3`.

## Licences

- Code: Clear BSD License (`LICENSE`). Keep `LICENSE` next to the kexts.
- `MtkBtFw.kext` embeds two MediaTek Bluetooth firmware files from linux-firmware (MT7925 and, new, MT7922
  `BT_RAM_CODE_MT7922_1_1_hdr.bin`). Both are under `LICENCE.mediatek`, not the Clear BSD License: redistribution is
  allowed "for the use with devices containing MediaTek chipsets", and not in a form that would put the file under the
  GPL or LGPL.
- `BtLeDlFix.kext` links Lilu's `plugin_start.cpp` (BSD 3-Clause): `LICENSE.Lilu.txt`. Lilu's name may not be used to
  endorse this project.
- Third-party details: `NOTICE.md`.

## Credits

| Project | What it gave this one |
|---|---|
| MediaTek firmware from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mediatek) | The MT7925 and MT7922 Bluetooth firmware embedded in `MtkBtFw.kext`, under `fw/LICENCE.mediatek` |
| [hbyonly1](https://github.com/hbyonly1) | MT7922 support (two commits, authorship kept) |
| Linux `btmtk` / `btusb` (`drivers/bluetooth`) | The firmware download procedure that `MtkBtFw` follows (re-written, not copied) |
| [acidanthera/Lilu](https://github.com/acidanthera/Lilu), [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM), [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK), [OpenCore](https://github.com/acidanthera/OpenCorePkg) | As in `bt-v1.1` |
| [Claude](https://claude.com/claude-code) (Anthropic) | Wrote most of the code and documentation, directed and tested by thanhnndev |
