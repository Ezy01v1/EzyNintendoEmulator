# N64: skipping the RSP's DMA wait loops (2026-11-17)

## What the RSP spends its instructions on

A temporary PC histogram of the interpreter (MK64, fields 0–1900, `--audio-hle --rdp-threaded`,
removed afterwards) counted 522.5 M RSP instructions. The top three are one loop:

```
0x12C  mfc0 $t3, SP_DMA_BUSY
0x130  bne  $t3, $zero, 0x12C
0x134  nop
```

13.7 % of all RSP instructions. The same loop on `SP_DMA_FULL` at 0x140–0x148 adds 1.2 %.

## The change

`RspRegs::run` recognises `mfc0 rt, SP_DMA_FULL|SP_DMA_BUSY` / `bne rt, $zero, -2` (either
operand order) / `nop` when the register reads nonzero, and jumps over every whole iteration
whose NOP would still run before `min(now + 1, others_bound())`. Nothing the loop reads can
change before the next SP DMA event, which is never before that bound, so registers, PC,
step parity (1/2 cycles in turn: iterations of 4 and 5 cycles) and the RSP's next step time
are exactly those of stepping. Single-step mode and a stop on an already raised MI line
keep the normal path. `step()` and the JIT are unchanged.

Diagnostics: `RspRegs::skipped_dma_wait_iterations()`, printed by the runner.
Over fields 0–1900: 20,772,439 iterations skipped (~62 M instructions, most of the loop).

## Validation

- New test `test_rsp_dma_wait.cpp`: chained DMAs with a FULL wait and a BUSY wait, three
  parities, five scheduler bound steps and three event periods; `run()` must match `step()`
  in the serialized RSP + MI state and the next event time after every bound. Two deliberate
  mutations (swapped parity, off-by-one bound) make it fail.
- Seven ctest suites pass. n64-systemtest: 2984/3721 (737 known failures), unchanged.
- Race image (`4B16…`) and final state (`007E…`) identical to the baseline in every run.

## Speed (i7-4790, race fields 1500–1900, `--audio-hle`)

Threaded RDP, four alternating pairs (fields/s, baseline → new): 38.7 → 35.8, 39.6 → 39.4,
39.8 → 38.7, 38.8 → 38.0. No gain visible: this mode is bound by more than the RSP loop.

Synchronous RDP, three alternating pairs (CPU s): 15.27 → 13.45 (noisy baseline),
13.38 → 13.30 (−0.6 %), 13.53 → 13.30 (−1.7 %).

The loop was 13.7 % of RSP *instructions* but those are the cheapest ones, so in time it
was ~1 % of the main thread. Kept because it is exact, tested and costs one mask compare
per instruction; do not present it as a measurable speed-up, nor repeat it as pending.
