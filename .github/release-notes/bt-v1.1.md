# bt-v1.1 (macOS 26 Tahoe): Bluetooth kexts

**Bluetooth kexts for the MediaTek MT7925 (USB `0489:e11e`) on macOS 26 Tahoe.** `MtkBtFw.kext` loads the Bluetooth
firmware over USB; `BtLeDlFix.kext` (a Lilu plugin) patches `bluetoothd` in memory so BLE devices connect and LE
advertising starts. For macOS 15 use `bt-v1.0`.

> ## Warning: vibe-coded, one-laptop project
> Almost all of this code was written by an AI (Claude) and tested on **one** ASUS ROG Zephyrus G14 **GA403WM** on
> macOS 26.7.2 (`25G332`, Darwin 25.6). The kexts run in the macOS kernel and can panic or hang the machine.
> `BtLeDlFix` patches `bluetoothd` in memory. Nobody has audited any of it. No warranty. Back up first.

## What this release is

- Tag target: `668f400` on branch `fix/bt-waitquiet`. It is **not** the tip of that branch: the tip carries MtkBtFw 0.4.0,
  which was built and never run.
- The zips are the files that ran on the test machine (boots of 2026-11-25 19:46 and 20:42), copied unchanged from its EFI.
- The Wi-Fi kexts in the tree at `668f400` are older than the ones in `v2.0-beta.1`. Use this tag for Bluetooth only.
- These notes live on the branch `release/v2.0-beta.1`, not in the tagged tree.

## What changed since `bt-v1.0` (2026-11-02, macOS 15 only)

Status words: **verified** = seen working on the GA403WM, with the date; **loaded** = runs daily, the specific effect
was not checked on its own; **compiles only** = built, never loaded.

| Item | Version | Status |
|---|---|---|
| LE advertising starts (AirDrop advert on air); the BLE mouse works on macOS 26 | BtLeDlFix 1.2.0 | verified 2026-11-05 |
| Build-specific bytes masked in the macOS 26 patterns (advertising error loop "STATUS 718" on `25G332`) | BtLeDlFix 1.3.0 | verified boot 2026-11-08 21:51: 0 lines "STATUS 718", `bluetoothd` at 0.9 to 1.9 % CPU |
| Per-patch hit counters, `sysctl debug.btledlfix` | BtLeDlFix 1.4.0 | loaded since 2026-11-08; no separate result recorded |
| `tools/btledl-check.py`: which patches match this macOS build | n/a | all four patches `applied` on `25G332` |
| A firmware left hung by a warm reboot is reset and reloaded (ended a `bluetoothd` restart loop) | MtkBtFw 0.2.0 | verified boot 2026-11-08 21:51 |
| IOKit is held busy during the 5.2 s firmware load, so `bluetoothd`'s one USB scan comes after it | MtkBtFw 0.3.0 / 0.3.1 | verified boot 2026-11-23 14:33: no `ExcUserFault_bluetoothd` |

Not in this release:

- MtkBtFw 0.4.0 (device-level load, no busy hold): compiles only, parked on 2026-11-23.
- Retry of a failed firmware download (branch `fix/bt-download-retry`): not merged.

## Known issues

- **MtkBtFw 0.3.1 delays the login window by 4 to 9 s** (WindowServer waits for the same registry quiet). Off switch:
  boot-arg `mtkbtbusy=0`.
- `BtLeDlFix` matches byte patterns in `bluetoothd`. If a future macOS 26 build matches the "advertising sets" pattern
  but misses the random-address patterns, the 718 loop returns. Run `tools/btledl-check.py` after every macOS update.
- Keyboard pairing, and re-pairing after a restart: not recorded.
- The versions in this release were run on `25G332` only.
- AirDrop needs the AWDL work of the Wi-Fi driver, which is not usable yet.

## Install

[GUIDE §13](https://github.com/thanhnndev/mt7925-macos/blob/dev/awdl/docs/GUIDE.md#13-bluetooth) ·
[BLUETOOTH.md](https://github.com/thanhnndev/mt7925-macos/blob/fix/bt-waitquiet/docs/BLUETOOTH.md).
Also needed, not included and not ours: [Lilu](https://github.com/acidanthera/Lilu) and `BlueToolFixup.kext` from
[BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM/releases), plus two NVRAM values (see the guide).

## Assets

Both kexts are the binaries that ran, byte for byte (`Info.plist` and executable compared with the EFI copies), zipped with
`ditto -c -k --keepParent`. Their symbol tables contain the build paths of the machine that built them.

| File | Version | Source | UUID of the binary inside | SHA-256 |
|---|---|---|---|---|
| `MtkBtFw.kext.zip` | 0.3.1 | `668f400` | `D173A506-D451-3CF2-A8AA-3F87414E1282` | `27e21cb36b3594bd731d68d21c809ccd611b999aa283a4ed842ac396a5a01fe6` |
| `BtLeDlFix.kext.zip` | 1.4.0 | `0a23a6c` (same source at `668f400`) | `C6274348-4A25-36D0-84D7-2E234AF13B8B` | `a2068e900cfcefa70fa37f291827a69129c85cdb28f6056f70809f8372bd5ed9` |
| `LICENSE` | n/a | `668f400` | n/a | `18ea8e404a91378ceb54eaa1514626b83d2c25a0aa6f6da3dbd2d4264e869304` |
| `LICENCE.mediatek` | n/a | `fw/LICENCE.mediatek` | n/a | `a90d3f66704d85889945fec5525ea77622549da83aced1aac99828383f8f1805` |
| `LICENSE.Lilu.txt` | n/a | acidanthera/Lilu `LICENSE.txt` at `0249c3a` | n/a | `74c3ca48189b796b0d262866e67de8efba77a71516667ce68dc3a8f13c2a3c57` |
| `SHA256SUMS.txt` | n/a | the five files above | n/a | n/a |

SHA-256 of the binaries inside the zips: `MtkBtFw` `2082c187267fad90bed2879b8fbb55c139c5c9f89a0d901101656afff7b8897a`,
`BtLeDlFix` `cd2f6e0c9f70d8896b0e652ddcd1eff65ed1421f26c202348ead832f7e34802c`.

A rebuild of `668f400` gives the same loaded sections as the `MtkBtFw` that ran. For `BtLeDlFix` the rebuild differs from
the binary that ran in two bytes: the build date in Lilu's version string (`2026-11-08` in the one that ran).

## Licences

- Code: Clear BSD License (`LICENSE`). A binary redistribution must reproduce the copyright notice, the conditions and
  the disclaimer: keep `LICENSE` next to the kexts.
- `MtkBtFw.kext` embeds the MediaTek Bluetooth firmware from linux-firmware. It is under `LICENCE.mediatek`, not the
  Clear BSD License: redistribution is allowed "for the use with devices containing MediaTek chipsets", and not in a form
  that would put the file under the GPL or LGPL.
- `BtLeDlFix.kext` is built against the Lilu headers and links Lilu's `plugin_start.cpp` (Copyright (c) 2016-2018
  vit9696, BSD 3-Clause). Its condition 2 asks a binary redistribution to reproduce Lilu's copyright notice, conditions
  and disclaimer: that is `LICENSE.Lilu.txt`. Lilu's name may not be used to endorse this project.
- Third-party details: `NOTICE.md`.
