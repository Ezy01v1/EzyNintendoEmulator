# RDP pixel and texture experiments

**Outcome: neither candidate is retained.** Both produced the same complete
Mario Kart 64 race images and serialized states as the frozen `70e5e64` runner.
Neither improved CPU time consistently in three paired measurements. The
production RDP sources were restored to the validated baseline.

## Method

Each candidate was built with the Release preset and compared with the frozen
baseline runner in fresh processes. Both used the RSP interpreter and synchronous
RDP. The timed window was VI fields 1500–1900, after 1500 untimed fields. Pair
order was baseline/candidate, candidate/baseline, baseline/candidate. Each run
saved a BMP and a complete binary state; all 12 images and all 12 states have
their respective identical SHA-256 hashes. Timings are unprofiled whole-process
CPU seconds. The raw results remain in ignored local directories under
`build/blend-reciprocal/timings` and `build/texture-tap-state/timings`.

## Final blender division

The first candidate replaced three integer divisions per blended pixel with a
24-bit ceiling-reciprocal table indexed by the common blend denominator. For
denominators 1–62 and numerators 0–255 times the denominator, exhaustive
comparison against integer division found no mismatch. The existing seven Debug
suites passed. The candidate nevertheless slowed two pairs:

| Pair | Baseline CPU s | Candidate CPU s | CPU reduction |
| --- | ---: | ---: | ---: |
| 0 | 17.421875 | 18.093750 | -3.86% |
| 1 | 17.437500 | 18.062500 | -3.58% |
| 2 | 17.921875 | 17.750000 | +0.96% |

Median paired CPU reduction: **-3.58%**. The table and replacement division
were removed.

## Filtered texture taps

The second candidate decoded tile size, stride, base, palette and TMEM mask once
per filtered texture sample and shared them among its texel fetches. The COPY
path retained the same per-texel decode through a shared helper. The seven Debug
suites passed, including existing indexed, filtered, wrapping, mirrored, and
COPY cases. The first pair had an unusually slow baseline; the next two pairs
favored the baseline:

| Pair | Baseline CPU s | Candidate CPU s | CPU reduction |
| --- | ---: | ---: | ---: |
| 0 | 18.000000 | 15.984375 | +11.20% |
| 1 | 15.875000 | 16.046875 | -1.08% |
| 2 | 15.718750 | 15.921875 | -1.29% |

Median paired CPU reduction: **-1.08%**. This candidate was also removed. A
reverse-order batch would not rescue the lack of consistent gain in these three
pairs. The game image/state checks establish correctness for the measured scene,
not a performance benefit. Neither experiment changes the JIT, AVX2 or RDP
threading defaults.

The restored core passed all seven Debug suites (202 N64 cases and 62,774
assertions per RSP/RDP mode). Release runner and frontend were rebuilt, and a
120-field MK64 state matches the earlier scalar baseline byte for byte. The
historical systemtest was not rerun because the final production core source is
unchanged from the previously validated `70e5e64` baseline (2,984/3,721).
