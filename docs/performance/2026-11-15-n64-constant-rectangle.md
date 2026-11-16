# Joint RDP frequencies and constant flat rectangles

**Outcome: retained.** Reusing the packed result of the existing pixel writer
for eligible flat rectangles improves whole-process CPU time in five of six
paired race measurements. Median paired CPU reduction is **5.28%**. Both batch
medians favor the candidate; all three complementary pairs improve. There is
one 3.63% regression and substantial timing variation in the first batch, so the
17.14% best pair is not representative. This is a local race-window result, not
a guarantee for other scenes or PCs. Interpreter/synchronous RDP defaults remain.

## Measurement

Extended the optional, default-OFF counters to group primitive dispatches and
opaque-writer events jointly by kind, cycle, size and complete other-modes word.
Captured the original renderer with RSP interpreter and synchronous RDP through
VI fields 1500–1900, resetting after the warm-up drain. Instrumented timings are
not performance evidence. Raw data is in ignored `build/rdp-joint/`.

The instrumented image and complete state match the established race reference:

- Image SHA-256: `73FE12DEA0CB88AF24A2D34C8056E95F403C82292FC48C7D41144BC1D215E314`
- State SHA-256: `286EA966C7D47AD294AF5A8BC838F1C3846A5E1C69032C9FD3B987242EA28BAE`

All writer attempts in this scene use one cycle and RGBA5551. The largest groups:

| Kind | Other modes (hex) | Primitives | Writer attempts | Survived | Coverage 8 |
| --- | --- | ---: | ---: | ---: | ---: |
| Triangles | EF882CFF00552078 | 27,616 | 15,845,839 | 15,845,839 | 12,987,222 |
| Triangles | EF882CFF0F0A4000 | 800 | 15,360,000 | 15,360,000 | 15,204,314 |
| Flat rectangles | EF882CFF0F0A4000 | 200 | 15,248,200 | 15,248,200 | 15,248,200 |
| Triangles | EF88ACFF00553078 | 7,009 | 2,234,112 | 644,722 | 0 |
| Triangles | EF882CFF00553078 | 3,776 | 1,763,191 | 400,315 | 0 |

The flat rectangles alone account for about 30.2% of all 50,536,845 opaque-writer
attempts. Their shared mode enables force blend, disables framebuffer/depth
reads and alpha comparison, and selects no RGB dither. This supplies a concrete
opportunity to eliminate repeated blending calculations; it does not establish
the CPU share of those calculations.

## Candidate

After the existing once-per-rectangle combiner evaluation, use a constant-pixel
path only when all clipped edges are integer-aligned, no framebuffer/depth access
is enabled, RGB dither is disabled and alpha comparison cannot consume random
numbers. Full coverage is then 8 everywhere. With memory coverage fixed at 7,
memory color zero and shade zero, the existing pixel writer gives the same result
at every valid address. Calculate the first valid pixel using that writer, read
back its packed bytes, and stamp the same pattern across remaining valid pixels.
If the representative pixel is rejected, all pixels are rejected. Rows still
respect field selection and each row is clipped to complete pixels in RDRAM.

Fractional edges, depth, framebuffer reads, dither and random alpha comparison
retain the scalar path. No derived state is persisted or serialized. The first
pixel's duplicate host store has no emulated side effects. Shader formulas,
coverage destination, alpha handling, packing and noise progression are reused.

## Correctness checks

Added 512 deterministic comparisons against the scalar writer, covering successive
mode changes, both pixel sizes/cycle types, arbitrary blend/alpha/coverage
selectors, field selection, odd base alignment and short/empty memory spans.
Separate checks verify unsupported conditions defer without touching memory or
noise. All seven Debug suites pass: N64 204 cases / 64,330 assertions per mode.

The uninstrumented Release comparison uses the preserved original runner and
`benchmark-race.ps1`, three alternating cold-process pairs, the same 1500–1900
window, interpreter and synchronous RDP. Every process saves a BMP and complete
state. Evidence is kept under ignored `build/constant-rectangle/`.

An instrumented candidate run confirms activation for all 200 flat rectangles,
with clipped fixed-point bounds `(0,0)..(1276,956)`. The stamp counter reports
15,248,200 pixel events. Generic opaque-writer calls fall from 50,536,845 to
35,288,845 (one representative call per optimized rectangle). Its image and
complete state match the same reference hashes. No timing from this instrumented
run is used for acceptance. `RdpBounds` rows now expose clipped 10.2 bounds for
future attribution, and optimized stamps have separate counters from generic
writer calls.

## Uninstrumented results

The first batch starts baseline/candidate; the complementary batch uses
`-Reverse` and starts candidate/baseline. Order alternates within each batch.
Both batches use the exact same frozen baseline and candidate executables.

| Batch / pair | Baseline CPU s | Candidate CPU s | CPU reduction |
| --- | ---: | ---: | ---: |
| First / 0 | 15.500000 | 16.062500 | -3.63% |
| First / 1 | 17.687500 | 14.656250 | +17.14% |
| First / 2 | 16.250000 | 15.531250 | +4.42% |
| Complement / 0 | 16.125000 | 14.984375 | +7.07% |
| Complement / 1 | 15.656250 | 15.000000 | +4.19% |
| Complement / 2 | 16.015625 | 15.031250 | +6.15% |

First-batch median: +4.42%. Complementary median: +6.15%. Across all six pairs,
median reduction: +5.28%. Keep all observations, including the initial regression
and the unusually slow second baseline. All twelve race images and all twelve
complete states match the reference hashes above. Raw wall times and field rates
are preserved in each batch's `results.json`; fields are VI interrupts, not
necessarily newly drawn game frames. These results cannot be added to older
speedup percentages from different batches.

## Final validation

- Restored counters OFF and rebuilt Debug; all seven suites pass, including
  N64 204 cases / 64,330 assertions in each backend/threading combination.
- Rebuilt the Release runner and frontend for normal use.
- Historical n64-systemtest 2.1.0, RSP JIT, synchronous RDP, AVX2 OFF: unchanged
  737 failures out of 3,721, retaining the 2,984-pass baseline. This remains a
  known partially failing ROM, not an all-passing test suite.
- MK64 `--state-check 1900` passes the 1900+120-field continuation, transferring
  interpreter/synchronous state to JIT/threaded mode and comparing complete
  states and images after the continuation.

Logs: `final-tests.log`, `final-build-release.log`, `systemtest.log` and
`state-check.log` under ignored `build/constant-rectangle/`. No experimental JIT,
AVX2 or threaded-RDP default was enabled. The next optimization should start
with a fresh profile of this renderer, since eliminating these pixel-writer
calls changes the remaining work distribution.
