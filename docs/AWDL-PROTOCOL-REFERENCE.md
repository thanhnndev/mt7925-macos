# AWDL / AirDrop protocol reference (for phases 4 and 5)

Written 2026-10-26. Status: reference notes, **not verified on this hardware**. Every row is a claim from a third party until it is
checked against the 25G241 disassembly or a capture from a real device; the "Check against" column says where.

## Sources and licensing

Decision taken with the project owner on 2026-10-26: the GPL projects below may be **read for protocol facts only**. No code,
structure layout text, comments or identifiers are copied into this repository; the tables below restate facts (field meanings,
constants, timings) in our own words, and the driver code stays written from the Apple binary and our own captures. If this rule
is relaxed or tightened, change `docs/ROADMAP-AWDL.md` §1 together with this file.

| Source | Version read | License | Used for |
|---|---|---|---|
| OWL, Open Wireless Link project (https://github.com/seemoo-lab/owl) | commit 0ee3263, 2026-10-12 | GPL-3.0 | frame layouts, TLV list, timing, election rules |
| OpenDrop (https://github.com/seemoo-lab/opendrop) | commit 11fe7ba, 2026-08-17 | GPL-3.0 | the AirDrop layer on top of `awdl0` |
| M. Stute et al., "A Billion Open Interfaces for Eve and Mallory" and "One Billion Apples' Secret Sauce" (Seemoo / USENIX Security 2019) | published | papers | AWDL design, election, channel sequence |
| Linux mt76 (`drivers/net/wireless/mediatek/mt76`) | not read this round | ISC | firmware commands (sniffer, ROC) |

## 1. AWDL frame facts (OWL)

| Item | Value | Check against |
|---|---|---|
| Action frame category | 127 (vendor specific), OUI `00:17:f2`, type 8, version 1.0 (compat) | matches ROADMAP-AWDL §10.23 (already seen on the air) |
| Action subtypes | PSF = 0, MIF = 3 | 25G241 `IO80211AWDLPeerManager` |
| After the subtype | one reserved byte, then two 32-bit clocks: the sender's PHY TX time and the target TX time | same |
| BSSID of every AWDL frame | `00:25:00:ff:94:73` | already in the driver (`kAwdlBssidDefault`) |
| Management header | addr1 = destination (multicast for PSF), addr2 = source, addr3 = AWDL BSSID, duration 0 | |
| Data frame | 802.11 data (not QoS) with the same three addresses, then LLC/SNAP (`aa aa 03`, OUI `00:17:f2`, protocol id `0x0800`) | check the Family's data path |
| AWDL data header | 2-byte magic `0x0403` (little endian), 16-bit AWDL sequence number, 16 zero bits, then the ethertype big endian (`0x86dd` for IPv6) | check `IO80211AWDLPeerManager` TX |
| TLVs | 1-byte type, 2-byte little-endian length | |

TLV types that matter: 2 service response, 4 synchronisation parameters, 5 election parameters (legacy), 6 service parameters,
7 enhanced data rate capabilities, 12 data path state, 16 ARPA (the hostname), 17 IEEE 802.11 container, 18 channel sequence,
20 sync tree, 21 version, 22 bloom filter, 23 NAN sync, 24 election parameters v2. Types 0, 1, 8, 9, 10, 11, 13, 14, 15 are
marked deprecated by OWL.

## 2. Timing and channels (OWL, paper)

| Item | Value |
|---|---|
| Availability window (AW) | 16 TU (1 TU = 1024 us) |
| Presence mode | 4 AWs make an extended AW (EAW) of 64 TU |
| Channel sequence | 16 slots; each EAW uses one slot. Default plan: slots 0 to 7 on one 5 GHz social channel (149 in OWL's default), slots 8 to 15 on channel 6 |
| Social channels | 6, 44 and 149 (bit 0, 1 and 2 of the social channel map in the data path state TLV) |
| Multicast EAW | slots 0 and 10 of the sequence |
| PSF interval | master 110 TU, slave 440 TU |
| Guard time | 3 TU for unicast, 16 TU for multicast (OWL's choice) |
| Sync | the receiver stores "time of the last AW boundary" and the AW counter masked to a multiple of 4; the sender's TLV gives the time to the next AW and the counter |
| Sync TLV fields | next AW channel, TX down counter (TU to the next AW), master channel, guard, AW period, AF period, flags, AW lengths, extension limits, master address, presence mode, next AW sequence, AP alignment, then the channel sequence |

Our driver sends the PSF every 440 ms with a 100 ms remain-on-channel (`kAwdlPsfPeriodMs`, `kAwdlPsfRocMs`), which is close to
the slave PSF interval (440 TU is 450 ms) but is **not** aligned to an AW boundary. Open question: whether the 25G241 Family
drives the AW schedule itself through the AWDL ioctls (presence mode, sync parameters, channel sequence) so that the driver
only has to place frames on the right channel at the right time. Read the ioctl handlers before writing any scheduler.

## 3. Election (OWL, paper)

* Each node has a counter and a metric. Initial values in OWL: counter 0, metric 60.
* The master is the node with the higher (counter, metric); with equal values the shorter tree wins, then the higher MAC.
* A node does not adopt a peer as its sync parent if that would create a cycle (the peer already syncs to us) or push the tree
  past a height of 10.
* The election parameters v2 TLV carries: current master, the node we sync to, the master's counter, distance to the master,
  master metric, own metric and own counter (the counter grows while a node is master).
* OWL itself states its election is "probably not fully correct". Do not treat it as a specification.

## 4. AirDrop layer (OpenDrop)

| Item | Value |
|---|---|
| Interface | `awdl0` with an IPv6 link-local address |
| Discovery | mDNS service `_airdrop._tcp` over `awdl0` |
| Port | 8771 in OpenDrop's server |
| Transport | HTTPS (TLS, client certificates not verified by the sender), POST |
| Requests | `/Discover` (sender record data in a binary plist), `/Ask` (file metadata, icon), `/Upload` (a cpio archive, content type `application/x-cpio`) |
| Flags | the receiver advertises capability flags in the mDNS record; macOS default is `0x3fb`, bit `0x80` means `/Discover` is supported |
| Validation record | Apple-signed CMS blob proving the sender's Apple ID; without it only "Everyone" mode works |

## 5. What this changes in the plan

1. Phase 4 is probably smaller than the roadmap says if the Family already owns election, sync and the AW schedule: check the
   handlers of `setAWDL_SYNC_PARAMS`, `setAWDL_ELECTION_*` and `setAWDL_CHAN_SEQ` in 25G241 before building any of it in the
   driver.
2. The first thing to verify on hardware is whether our PSF is accepted by a real peer: look for the peer's PSF with our MAC in
   the sync tree, or for the peer appearing in `awdl0`'s neighbour table.
3. The data path needs: SNAP header with protocol id `0x0800`, the AWDL data header, IPv6 ethertype, and group-addressed frames
   only in multicast EAWs (slots 0 and 10).
4. AirDrop itself (phase 5) runs entirely above `awdl0`: macOS `sharingd` does the TLS, mDNS and cpio parts. The driver only has
   to make `awdl0` carry IPv6 unicast and multicast.
