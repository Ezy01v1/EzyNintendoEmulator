# Primitive-level dispatch for the one-cycle blender

**Outcome: both candidates rejected; original rendering paths restored.** The
indirect variant regressed all three pairs. The direct variant improved only
three of six pairs, with a +0.45% median paired CPU reduction and regressions up
to 5.65%. Neither provides a consistent whole-game speed benefit. The optional
November 14 diagnostic counters remain OFF; interpreter and synchronous RDP
defaults are unchanged.

## Candidate

Select a pixel writer once per flat rectangle, textured rectangle or triangle.
One-cycle RGBA5551 primitives with final P source equal to pixel color use a
template specialization; force-blend ON and OFF have separate instantiations.
Other modes use the original general algorithm. Pixel alpha, coverage, memory
reads, dithering and framebuffer storage retain their original arithmetic.
Dispatch uses a member-function pointer and introduces one indirect call per
pixel, so the reduced generic branching may not improve whole-game performance.
No new serialized or persistent derived state is introduced.

The existing diagnostic counter changes from November 14 remain independent of
this experiment. Counters are OFF for all measurements and tests below.

## Generated code

Matching Release PDB symbols were inspected with DbgHelp and the executable was
disassembled with the local Visual Studio dumpbin. The original pixel writer is
1,055 bytes and its outlined blend lambda is 761 bytes. Each specialized writer
is 780 bytes and its outlined lambda is 571 bytes. The compiler folds the two
specialized lambdas together: force-blend selection is still dynamic within
that shared function. The specialization removes size/cycle handling from the
outer writer and P-source selection from the blend lambda, but does not remove
all dynamic mode decisions. Sizes alone are not speed evidence.

## Validation method

Preserve the unmodified, counters-OFF Release executable before building the
candidate. Run the complete Debug suite, then compare Release executables using
`benchmark-race.ps1`, RSP interpreter, synchronous RDP, fields 1500–1900, three
alternating fresh-process pairs. Save and compare the complete serialized state
and image after every run. Keep other emulator and build workloads stopped.

Local evidence belongs to ignored `build/blender-dispatch/`: executable/PDB
pairs, build logs, DbgHelp symbol listings, disassembly and raw timing results.
Do not compare these timings with other PCs or earlier batches.

The candidate passes all seven Debug suites, including 202 N64 cases / 62,774
assertions in each of the four backend/threading combinations.

## Indirect dispatch: rejected

| Pair | Baseline CPU s | Candidate CPU s | CPU reduction |
| --- | ---: | ---: | ---: |
| 0 | 15.671875 | 16.062500 | -2.49% |
| 1 | 15.687500 | 16.140625 | -2.89% |
| 2 | 15.718750 | 15.796875 | -0.50% |

Median paired CPU reduction: **-2.49%**. Every pair favors the baseline. All six
images and all six complete states match their respective hashes from the
November 14 frequency experiment. Smaller specialized functions did not yield
faster execution with indirect per-pixel dispatch.

## Direct-call variant

To separate indirect dispatch overhead from specialization, the second candidate
templates the three primitive loops and selects their instantiations once per
primitive. Each loop calls its pixel writer directly. It uses the same guards,
pixel arithmetic and general fallback as the first candidate, with potentially
more code duplication. Measurements and validation use the same method.

This variant also passes all seven Debug suites (same N64 case/assertion counts).
Its PDB/disassembly confirms a direct call to the selected pixel specialization
from the triangle loop. Each of the three triangle instantiations is 6,699 bytes,
the same size as the original triangle routine, so this duplicates roughly
13.4 KB of triangle code alone. Pixel writer and outlined lambda sizes remain
780/571 bytes for the specialized path and 1,055/761 for the general path.

### Direct-call results: rejected

The first batch starts candidate/baseline (`-Reverse`), the complementary batch
starts baseline/candidate. Order alternates within each batch.

| Batch / pair | Baseline CPU s | Candidate CPU s | CPU reduction |
| --- | ---: | ---: | ---: |
| First / 0 | 15.515625 | 15.578125 | -0.40% |
| First / 1 | 15.796875 | 15.312500 | +3.07% |
| First / 2 | 15.484375 | 15.281250 | +1.31% |
| Complement / 0 | 15.531250 | 15.187500 | +2.21% |
| Complement / 1 | 15.531250 | 15.671875 | -0.91% |
| Complement / 2 | 15.500000 | 16.375000 | -5.65% |

The first batch's +1.31% median prompted the complementary batch; its -0.91%
median does not confirm the gain. Across all six pairs, the median paired
reduction is **+0.45%**. Removing indirect dispatch does not establish a useful,
repeatable benefit relative to the original code.

All 18 race images and all 18 complete states from both candidates match:

- Image SHA-256: `73FE12DEA0CB88AF24A2D34C8056E95F403C82292FC48C7D41144BC1D215E314`
- State SHA-256: `286EA966C7D47AD294AF5A8BC838F1C3846A5E1C69032C9FD3B987242EA28BAE`

## Follow-up

Final restoration validation: full Debug rebuild and seven suites passed (N64
202 cases / 62,774 assertions per mode). Release runner and frontend were
rebuilt with counters OFF. A paired 120-field boot check matches image and
complete state exactly. The production rendering source is restored, so the
historical systemtest was not rerun; its prior 2,984/3,721 result remains the
baseline rather than a new measurement of either discarded candidate.

Do not repeat these primitive-dispatch implementations without new evidence.
Measure joint mode frequencies and primitive type (especially flat rectangles),
rather than relying on marginal counts alone. A stronger candidate would remove
repeated arithmetic or memory work, not just selector/cycle branches. Neither
the first frequency report nor this experiment establishes which joint path
dominates total RDP time.
