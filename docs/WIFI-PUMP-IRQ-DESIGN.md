# Wi-Fi pump: why it polls, what it costs, and the interrupt design (2026-11-27)

Design only, on branch `design/wifi-irq` (from dev/awdl 823d9e4, the source of the live kext). No code was changed and nothing was
installed. The kernel_task poller list (ga403wm-power `docs/KERNEL-TASK-POLLERS.md`, row 2) is the origin.

## Why it polls

- `m1` bring-up writes `MT_WFDMA0_HOST_INT_ENA = 0` (`itlmt7925_m1.cpp:494`) and never enables it again. `MT7925_PCIE_MAC_INT_ENABLE`
  is set to 0xff as in Linux, so only the WFDMA host mask is closed.
- There is no `IOInterruptEventSource` and no `registerInterrupt`: the driver has no interrupt at all. Every RX completion, TXFREE
  (which comes on the RX data ring) and MCU event is found by reading ring indices from the `m3` pump thread.
- **The hardware path exists.** The WLAN@0 nub publishes two interrupt specifiers:
  - index 0 is the IOAPIC pin;
  - **index 1 is `IOPCIMessagedInterruptController`, i.e. MSI.**

  So nothing is missing on the macOS side.

## What the loop does today (`m3` link loop, `itlmt7925_m3.cpp:2647-2793`)

```
int act = m3PumpOnce();           // ring index MMIO reads, RX/TXFREE/MCU processing
... ~25 periodic checks (AWDL, scan, BA reorder 10 ms, A-MSDU 4 ms, TX stall 20 ms, pulse 5 s, stats) ...
if (act)                         { lastActive = now; IODelay(20); }
else if (now - lastActive < 50)  IODelay(200);    // 50 ms "keep polling" window after any frame
else                             IOSleep(1);
```

- `IODelay` longer than xnu's spin threshold blocks on a timer, so these are wakeups, not spins:
  - about 4-5k passes/s whenever a frame came in the last 50 ms;
  - 1000/s at rest. It never stops: `IOSleep(1)` runs forever while associated.
- Under the daily load the 50 ms window never closes, because frames arrive every few ms. The rates measured now (30 s, no root,
  `itlmt_rx_received` / `itlmt_tx_sent`) were **359 RX + 352 TX frames/s**.
- Each pass costs a thread wake plus block (two context switches), a timer interrupt on some core, and the loop body: several
  uncached MMIO reads of ring CPU/DMA indices plus the checks.

## CPU share, measured without root

- kernel_task as a whole: **14.6 % of a core** (top, 30 s), and **33.6k context switches/s** (top CSW delta, 20 s). Docker stack +
  video + Claude were up.
- Boot of 20:00 (out-54), 20:05, 30 s, Docker stack just started: **4281 RX + 321 TX frames/s**, kernel_task **14.9 %** (TIME
  34.68 → 39.14 s), **41.6k context switches/s**.
  - At this rate nearly every pass finds work, so the loop runs the `act` branch: `IODelay(20)`, a ~20 µs timer block plus wake
    latency.
  - That allows ~15-25k passes/s, not 4.5k. The pump's share grows with traffic, and it is likely the largest part of the 41.6k
    switches/s.
- The per-thread split of kernel_task needs root. It is `sudo ~/develop/projects/ga403wm-power/tools/kt-probe.sh 30` (`ps -M -p 0`
  plus a spindump), which names the pump thread's CPU % exactly.
- Estimate until then:
  - ~4.5k passes/s × (2-3 µs switch + 5-8 µs body) ≈ **3-5 % of a core**, plus ~4.5k timer pops/s;
  - about 9k of the 33.6k switches/s;
  - the pump's core never stays in CC6;
  - at rest (no traffic) 1000 wakeups/s, about 1 %.

## Measured with root (kt-probe, 20:22, 30 s, 10 ms spindump plus powermetrics)

- kernel_task: 102-118 ms/s in powermetrics (10-12 % of a core), with ~4.66k interrupt wakeups/s. The thread CPU times in the spindump
  add up to 2.18 s per 30 s (7.3 %); the rest is interrupt time.
- Busiest threads:

  | thread | CPU |
  |---|---|
  | unnamed 0x1b5, priority 81 | 0.427 s (1.4 %) |
  | `thread call high #9` (NMXGPU) | 0.412 s |
  | `VoodooI2CDeviceNub` | 0.364 s |
  | `thread call kernel-high #3` | 0.139 s |
  | `dlil_input_en1` | 0.096 s |
  | `IO80211RangingManager` | 0.084 s |

- 0x1b5 spends nearly all its samples blocked in `thread_block_reason`, with a `delay_for_interval` frame (IODelay), and every frame
  above the kernel is unresolved ([0x0], an AuxKC kext). That matches the pump. The named `itlmt7925Wifi` work-loop thread had not
  run for 1365 s.
- So the pump thread itself is **≤ 1.4 % of a core**, plus its share of the timer interrupts. That is lower than the 3-5 % estimate
  above. Design B saves about 1 %, and A saves that plus the timer wakeups and idle C-state time.

## Design A: MSI with a NAPI-style loop (the real fix)

What Linux mt7925e does: one MSI vector. The hard IRQ writes `HOST_INT_ENA = 0` and schedules a tasklet. The tasklet reads and
acks `HOST_INT_STA` and runs NAPI on the rings whose done bits are set; each poll takes up to 64 frames. When a poll finishes under
budget, it re-enables those bits.

macOS port:
1. In `start()`, find the MSI index: the specifier whose controller is `IOPCIMessagedInterruptController`, via `getInterruptType` ==
   `kIOInterruptTypePCIMessaged`. Create an `IOFilterInterruptEventSource` on it.
   - The filter runs at primary interrupt level and does only one MMIO write: `HOST_INT_ENA = 0` (mask). It returns false: no
     work-loop action.
   - It then wakes the pump thread with `thread_wakeup_one(&pumpEvent)`, or `semaphore_signal` from the filter.
   - The pump keeps its own thread, so no IOWorkLoop gate enters the data path.
2. Pump wait. It replaces the `IODelay`/`IOSleep` tail:
   - Before blocking, the pump re-enables the ring done bits (RX data, RX MCU/WM, TX MCU done) and then checks the ring indices
     once more. This closes the race of a frame landing between the last pass and the unmask.
   - It then blocks with `assert_wait_deadline(&pumpEvent, THREAD_UNINT, nextTimerDeadline)` + `thread_block`.
   - `nextTimerDeadline` is the earliest of the timers the loop still needs:
     - BA reorder 10 ms (only while a BA window holds frames);
     - A-MSDU flush 4 ms (only while one is open);
     - TX stall retry 20 ms (only while `ethStalled`);
     - AWDL hold or next window;
     - pulse 5 s, stats, and beacon-loss checks: 1 s granularity is enough.
   - With nothing pending this is about 1 wakeup/s.
3. On wake, the pump reads `HOST_INT_STA`, acks it (W1C), and processes the rings as `m3PumpOnce` does now, with a budget per
   pass. While passes keep finding work (`act`), it loops without sleeping or unmasking, as NAPI does. When a pass is empty, it
   goes back to step 2.
4. TX side:
   - `outputPacket` already writes descriptors and kicks DMA from the output queue thread, so TX needs no pump wakeup.
   - Exception: when the TX pool is full (`ethStalled`), the output path wakes the pump so TXFREE is reaped at once.
   - AWDL data queued for an on-channel send also wakes the pump.
5. The bit masks come from upstream `mt7925/pci.c` `irq_map` and `mt792x_dma` headers (RX data done, RX WM done, TX MCU done, MCU
   command sw int). They must be copied from that source and are not in `mt7925_regs.h` yet.
6. Switch and fallback:
   - Boot-arg `itlmt_irq=0` keeps today's polled loop. The name was checked: no installed kext uses it.
   - Watchdog: if a ring index shows unprocessed descriptors for 100 ms while no interrupt came, log it once, count it, and fall
     back to polling for that link. A silent missing interrupt then costs throughput, not the link.
7. Sleep/wake and reset: mask (`HOST_INT_ENA = 0`) and `disable()` the source before `m3Teardown`, chip reset and system sleep.
   Re-enable it after `m1` bring-up.

Expected effect:
- At rest: ~1 wakeup/s instead of 1000.
- Under daily traffic: one wakeup per interrupt burst. The firmware coalesces RX done per A-MPDU, so this is hundreds per second
  instead of ~4.5k, and each wakeup does real work.
- CPU saving ≈ the measured pump share minus the real work: **~3-4 % of a core, ~0.2-0.4 W**. The idle C-state gain on that core
  comes on top.
- Throughput must be re-measured against the 29-36 MB/s baseline (native round 26d). NAPI-style batching should hold it.

Risk:
- An MSI that never fires on this platform. The Strix xHCI showed a single-vector quirk, so this is not hypothetical. The watchdog
  fallback in step 6 contains it.
- The first test boot logs: interrupts/s, passes per interrupt, and fallbacks.

## Design B: adaptive sleep without an interrupt (stopgap, small diff)

This changes only the loop tail. Nothing in bring-up changes.

```
if (act) { lastActive = now; backoffUs = 50; IODelay(20); }            // busy: as now
else if (now - lastActive < 50) { IODelay(backoffUs); backoffUs = min(backoffUs * 2, 1000); }   // 50, 100, 200, 400, 800, 1000 us
else IOSleepWithLeeway(1, 1);                                            // idle: 1 ms, the timer may coalesce up to 2 ms
```

- RX ring headroom: 1536 slots ≈ 60 ms of frames at the measured peak (~25k frames/s at 36 MB/s), so a 1 ms cap cannot overflow.
- The worst added RX latency is 1 ms after a gap. The TCP ACK clock at 360 frames/s already spaces frames ~3 ms apart.
- Expected:
  - under the daily load, ~1-1.5k passes/s instead of ~4.5k;
  - at rest, ~500-1000 wakeups/s instead of 1000, with coalescing (the code uses `IOSleepWithLeeway(1, 1)`);
  - **~2-3 % of a core**, no interrupt risk.
- Must be checked: iperf/`networkQuality` throughput (29-36 MB/s baseline), ping jitter to the AP, and the AirDrop/AWDL on-channel TX
  (§10.89).

## Recommendation

Build B first: one hunk, no new hardware path, and its gain is measured with the same counters. Do A next on its own branch,
because A is the one that takes the pump to ~0 at rest. Both go through the Wi-Fi owner's review and main's build/install. Before
either, run the root `kt-probe.sh 30` once, under the daily load, to replace the estimated pump share with the measured one.
