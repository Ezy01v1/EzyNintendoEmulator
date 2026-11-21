# RDP combiner state predecode experiment

**Outcome: rejected.** Decoding selectors and uniform colors when RDP commands
changed them produced identical MK64 race images and complete save states, but
increased CPU time in two of three paired runs. The production rasterizer was
restored; the prior direct hidden-DZ optimization remains intact.

## Candidate

The candidate stored both cycles' eight combiner selectors and unpacked primitive,
environment, key center/scale/width, K4/K5, and primitive LOD fraction in a derived
structure. `Set Combine`, `Set Primitive Color`, `Set Environment Color`, `Set Key
R/GB`, and `Set Convert` rebuilt it. State loading rebuilt it after deserializing
the original registers, without adding fields to the save-state format. Default
construction and rasterizer reset gave the same zero state as the original fields.
The per-pixel equation, two-cycle texel routing, chroma key, and noise advance were
unchanged. The experimental core source was reverted after measurement.

## Validation and measurement

The `benchmark-race.ps1` baseline was the frozen Release runner from `70e5e64`;
the candidate was built with Release preset. Both used the RSP interpreter and
synchronous RDP, with a fresh process for each run. The timed MK64 race window was
VI fields 1500–1900 after untimed warmup. Order was baseline/candidate,
candidate/baseline, baseline/candidate. `build/combine-predecode/timings/results.json`
contains the six complete local records. All six BMP and serialized-state SHA-256
hashes match exactly.

| Pair | Baseline CPU s | Candidate CPU s | CPU reduction |
| --- | ---: | ---: | ---: |
| 0 | 17.359375 | 18.140625 | -4.50% |
| 1 | 17.390625 | 17.343750 | +0.27% |
| 2 | 17.468750 | 17.843750 | -2.15% |

The median paired CPU reduction is **-2.15%**. Wall time likewise worsened in
all three pairs. A reverse-order batch was unnecessary to reject this candidate;
these measurements make no claim about other workloads or hardware.

The new tests that exercise immediate updates of combiner selectors and uniforms
remain as regression coverage. The initial candidate and restored core passed all
seven Debug suites. The restored core passes 202 N64 cases / 62,774 assertions per
RSP/RDP mode; the rebuilt Release runner's 120-field state matches the earlier
scalar baseline byte for byte. The Release frontend was also rebuilt. The
historical systemtest was not repeated because the final core source is unchanged
from the previously validated `70e5e64` baseline (2,984/3,721).

Next measured areas from the previous RDP profile are opaque pixel writing
(about 8.23% exclusive samples) and texture sampling (about 5.62%).
