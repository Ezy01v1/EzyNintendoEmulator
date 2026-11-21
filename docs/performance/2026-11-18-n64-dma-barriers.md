# N64: range barriers for PI and SI DMA (2026-11-18)

## Where the main thread waited

With the graphics HLE on, MK64's race (fields 1500–1900, `--audio-hle --rdp-threaded --gfx-hle`)
ran at ~45 fields/s while the RDP thread was busy only ~60 % of the time (process CPU 14.1 s
in 8.8 s of wall time). The main-thread profile (`--profile`) showed ~29 % of its samples
waiting: `ZwYieldExecution` 19.7 %, `Rdp::prepare_memory` 8 %, `RdpWorker::wait` 1 %.

A temporary log of every RDP wait (removed afterwards) attributed them:

| | PI drains | SI drains | range waits | time waiting |
|---|---|---|---|---|
| LLE graphics | 80 | 39 | 43 | 0.08 s |
| graphics HLE | 458 | 182 | 0 | **~5.0 s** |

Writing `PI_RD_LEN`/`PI_WR_LEN` or starting an SI DMA drained the whole RDP queue. The HLE
hands the RDP a whole frame's commands when the task starts, so a cartridge or controller
DMA right after it waited for the frame to be drawn; with the RSP running the microcode the
commands trickle in and the queue is short at that point.

## The change

`Bus::prepare_pi_dma` and `Bus::prepare_range` wait only for the RDP packets that touch the
DMA's RDRAM range, like the CPU, the VI and the audio DMA already do:

- PI cartridge → RDRAM: written from `PI_DRAM_ADDR & ~7` for the length plus up to 8 bytes
  per 128-byte block (`Pi::copy_to_rdram` ends each block on an 8-byte boundary). A write
  barrier: it also waits for the RDP's readers of those pages (textures).
- PI RDRAM → cartridge: a read barrier over the same range (no RDRAM read is emulated yet,
  but SRAM/Flash would need it).
- SI: 64 bytes at `SI_DRAM_ADDR`, written by PIF → RDRAM, read by RDRAM → PIF.
- A range leaving RDRAM still drains everything.

## Validation

- Eight suites (Debug), including the threaded N64 suites.
- Full machine state after 1900 fields identical between synchronous and threaded RDP, with and
  without the graphics HLE (`--dump-state`).
- Cross-mode continuation 1700+120 (`--state-check`) passes with and without the graphics HLE.
- n64-systemtest with `--rdp-threaded`: 2984/3721, as before.

## Speed

Interleaved pairs, old and new binaries alternating which runs first, same session:

| | old | new |
|---|---|---|
| graphics HLE (5 pairs) | 43.7, 44.5, 43.2, 44.8, 45.2 | 51.9, 51.8, 51.5, 51.6, 51.6 |
| LLE graphics (3 pairs) | 43.9, 44.2, 43.9 | 44.2, 43.4, 43.8 |

Graphics HLE: median **44.5 → 51.6 fields/s (+16 %)**, 5/5 pairs favourable. LLE graphics: no
change. Unpaired runs earlier in the same session varied from 38 to 52 fields/s; only the
pairs above are comparable.

## Follow-up: cheaper barrier checks

With the drains gone the main thread is the limit, and its profile still gave
`Rdp::prepare_memory` 13.4 % of the samples with a single real wait in 400 fields: the cost of
checking. Every CPU access to RDRAM called `RdpWorker::pending()`, which reads each band's
`completed` counter with acquire loads; the RDP threads write those counters after every
packet, so the cache lines kept moving between cores.

`wait_memory()` now looks up the pages' newest packet in the producer's own tables first and
compares it with `completed_seen_`, the producer's last reading of `completed()`. Only a page
with a newer packet reloads the counters (and waits if needed). `failed_` is still checked on
every call, so a worker failure is rethrown as before (`test_rdp_worker.cpp`). Giving `failed_`
its own cache line was not needed.

Validation: eight suites; full states at 1900 identical to the synchronous RDP's, with and
without the graphics HLE; cross-mode continuation 1700+120 with and without it.

Interleaved pairs (old = range barriers above, new = this change):

| | old | new |
|---|---|---|
| graphics HLE (5 pairs) | 51.7, 51.7, 52.1, 52.3, 51.9 | 54.0, 50.8, 55.1, 52.9, 53.8 |
| LLE graphics (3 pairs) | 43.5, 43.4, 43.4 | 44.7, 43.0, 44.5 |

Graphics HLE: median 51.9 → **53.8 fields/s (+3.7 %)**, 4/5 favourable. LLE graphics: median
43.4 → 44.5 (+2.5 %), 2/3. Small but repeatable; keep comparing in pairs.
