# RDP branch frequencies in the Mario Kart 64 race

## Outcome

Added optional compile-time RDP counters, OFF by default. The measured scene
strongly favors one-cycle RGBA5551 writes and filtered textures. No rendering
optimization is included and no speed improvement is claimed. The next candidate
should target a common one-cycle pixel path rather than retrying combiner input
caches, reciprocal division or shared texture-tap decoding.

## Method and validation

The existing Release runner was frozen before rebuilding with
`EZY_N64_RDP_COUNTERS=ON`. Both processes used the local MK64 ROM, RSP interpreter
and synchronous RDP, warming up through 1500 VI fields and finishing at 1900.
Counters reset after the warm-up drain and include the measured loop and final
drain only. Raw logs, CSV, executables, BMPs and states are in ignored
`build/rdp-frequency/`. Reproduction commands are in `tools/n64_runner/README.md`.

The instrumented and frozen runs have identical SHA-256 hashes:

- Image: `73FE12DEA0CB88AF24A2D34C8056E95F403C82292FC48C7D41144BC1D215E314`
- Complete state: `286EA966C7D47AD294AF5A8BC838F1C3846A5E1C69032C9FD3B987242EA28BAE`

Threaded benchmark collection rejects with exit code 2. The paired timing script
rejects instrumented JSON using its new `rdp_counters` field. These paths were
checked with a one-field invocation. Existing runners without that field remain
accepted. Counter timings are intentionally excluded from performance evidence.

With counters restored to OFF, the full Debug build and all seven test suites
passed (202 N64 cases / 62,774 assertions in each backend/threading combination).
Release runner and frontend were rebuilt with counters OFF. A final paired
120-field smoke run accepted both old/new JSON formats and matched image and
complete state hashes. That short boot check is not race performance evidence.

## Frequencies

Counts are events at each named stage, not unique screen pixels. Rows with
different denominators must not be added together.

| Stage | Count | Meaning |
| --- | ---: | --- |
| Opaque write attempts | 50,536,845 | All cycle type 0 (one cycle), pixel size 2 (RGBA5551) |
| Alpha rejection | 0 | Rejections within those attempts |
| Zero coverage rejection | 2,952,266 | 5.84% of attempts |
| Reached final blender | 47,584,579 | Final P input always pixel color |
| Image read enabled | 16,976,379 | 35.68% of surviving writes |
| Force blend enabled | 30,693,703 | 64.50% of surviving writes |
| Final blend bypass | 16,469,393 | 34.61% of surviving writes |
| Final blend executed | 31,115,186 | 65.39% of surviving writes |
| RGBA texture samples | 19,810,425 | All filtered; palette-type bit 46 always zero |
| RGBA16 texture samples | 17,576,313 | 88.72% of texture samples |
| CI8 texture samples | 2,234,112 | 11.28% of texture samples |
| Depth mode opaque | 19,843,444 | Among calls with Z comparison enabled and valid address |
| Depth mode interpenetrating | 4,624 | Same denominator |
| Depth mode transparent | 80,879 | Same denominator |
| Depth mode decal | 0 | Same denominator |

These are marginal counts. In particular, image-read and force-blend counts do
not establish their joint distribution. Texture filtering counts do not separate
three-tap interpolation from the four-tap midpoint case. COPY/FILL raw writes,
unsupported operations and work outside this race window are not represented by
the opaque-write and texture-sample counters.

## Next experiment

Investigate dispatching a common one-cycle RGBA5551 blender path at primitive
setup, keeping the general path for other modes. All final P inputs here are
pixel color, and two-cycle work is absent, so this could remove repeated generic
selector/cycle branches. Frequency alone does not establish that this is costly
or that specialization will help: code size and inlining may erase the benefit,
as earlier experiments showed. Inspect generated code and measure any candidate
with counters OFF, alternating baseline/candidate processes and comparing every
image and complete state. Keep interpreter and synchronous RDP defaults.
