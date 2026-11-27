# MT7925 firmware

Three blobs from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mediatek/mt7925),
directory `mediatek/mt7925/`, unchanged. The two Wi-Fi blobs are compiled into `itlmt7925.kext` (`kexts/itlmt7925/fw_blob.c`, `.incbin`), the Bluetooth one into `MtkBtFw.kext` (`kexts/MtkBtFw/fw_blob.c`)
because nothing can be read from disk in macOS Recovery at the time the driver starts.

| File | Version (linux-firmware `WHENCE`) | SHA-256 |
|---|---|---|
| `WIFI_MT7925_PATCH_MCU_1_1_hdr.bin` | 20260813113015a | `8eb46014d2a6b4124472eee7476d995008a6f40b1daffef87eb42f30d98699e1` |
| `WIFI_RAM_CODE_MT7925_1_1.bin` | 20260813113118 | `23ff53b4bb639b30481e2e06bb1688569ad1ba971b897936db539882abfbd120` |
| `BT_RAM_CODE_MT7925_1_1_hdr.bin` (Bluetooth, compiled into `MtkBtFw.kext`) | 20260813113236 | `be7c18e37221e277baaef853be3a0ba930138ee879f336ff7b185e79ab09a5c9` |
| `BT_RAM_CODE_MT7922_1_1_hdr.bin` (MT7922 Bluetooth, experimental, compiled into `MtkBtFw.kext`; from linux-firmware `mediatek/`, added by hbyonly1) | header date 20260724143815 | `299942dbec7d34af030670ea59f9f7b4fd5c64e738b9e0157d76f4dd3beb190f` (byte-identical to linux-firmware `mediatek/BT_RAM_CODE_MT7922_1_1_hdr.bin` at commit [`f997b28`](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/commit/?id=f997b2832a0fbdfaf9b9158e9972e2eb3d383af8), "update bluetooth firmware to 20260724143815"; checked by the maintainer 2026-11-27. linux-firmware has a newer 20261027203403 build since) |

License: [`LICENCE.mediatek`](LICENCE.mediatek) (copied verbatim from `LICENSES/LICENCE.mediatek` of linux-firmware). It permits
redistribution for use with devices containing MediaTek chipsets, provided the files are not combined into something that
requires them to be licensed under GPL/LGPL. The Clear BSD License of this repository does **not** apply to these files.

To use a newer firmware, replace the files with the ones from linux-firmware and rebuild. The driver parses the region
table of both images and waits for the firmware-ready bit, but a newer firmware may change command formats; the
driver has only been run against the versions above.
