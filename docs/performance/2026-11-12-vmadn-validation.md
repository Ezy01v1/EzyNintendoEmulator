# VMADN AVX2 validation on the second PC

The unfinished `00ca4e6` checkpoint was resumed on 2026-11-12. The kernel is
arithmetically correct in the exercised cases, but did not demonstrate a
whole-game speed improvement. `EZY_N64_VECTOR_AVX2` now defaults to OFF; it
remains available for experiments. The interpreter and synchronous RDP remain
the runtime defaults. No expansion to VMADM/VMADH or merge to main is justified
by these results.

## Correctness and builds

All builds used the repository's CMake presets, sequentially, with
`--parallel 1 -- /nr:false /p:CL_MPCount=1`. Existing dependencies were reused
with `FETCHCONTENT_FULLY_DISCONNECTED=ON` after fetching pinned AsmJit.

- AVX2 OFF: all four N64 Debug suites passed, each with 193 cases and 48632
  assertions, exercising both RSP backends and both RDP modes.
- AVX2 ON: all seven Debug suites passed. Each N64 combination passed 194
  cases and 69112 assertions, including the direct kernel and public dispatch
  tests for signedness, 48-bit wrap, saturation, selectors, aliases, flags and
  division state.
- Release runner, vector benchmark and frontend built successfully. The
  benchmark explicitly reported `VMADN kernel: AVX2` on this PC.
  After evaluation, OFF was restored in the local preset cache and all three
  Release targets rebuilt. The final benchmark reports `scalar`; the final
  interpreter runner matches the reference image at 300 fields.
- The 30 benchmark rows have identical scalar/AVX2 state checksums. The
  benchmark script now rejects mismatched identities, instruction counts or
  checksums when comparing a candidate with `build/vmad-before.csv`.
  A deliberately altered reference checksum was rejected; original CSVs were
  restored afterward.
- Full MK64 saved states match byte for byte at 120, 300 and 600 fields
  (interpreter, synchronous RDP), regenerated on this PC with AVX2 OFF/ON.
- The JIT/AVX2 state at 600 fields also matches the scalar interpreter reference.
  Cross-mode save/load continuation passes after 300+120 fields in both
  directions, switching both RSP and RDP backends and comparing complete states
  and images.
- All twelve 300-field game images below match exactly, across both RSP
  backends and both kernel variants. SHA256:
  `58F97E8061E7B69739CC69989AC679148BE087E402EA9A6C5A9AE5C36E1512FB`.
- The historical local n64-systemtest ROM (2.1.0, base set) finished with
  **737 failures out of 3721**, retaining the **2984/3721** passing baseline,
  with AVX2 ON, JIT and synchronous RDP. Invocation:
  `ezy_n64_runner roms/n64/n64-systemtest.z64 --rsp-jit --rdp-sync --isviewer 60`.
  The runner exits 1 because the known baseline failures remain; the summary,
  not a zero exit code, establishes the unchanged result. Output is preserved
  in `build/vmad-validation/systemtest-avx2-jit.log`.

## Whole-game measurements

MK64, 300 fields from a cold process, synchronous RDP, Release/LTCG. Three
adjacent pairs per backend in scalar/candidate, candidate/scalar,
scalar/candidate order. No simultaneous builds or other emulator tests.
CPU time is the child process's total processor time; wall time includes
process startup and screenshot output. These are title-sequence measurements,
not a claim about race performance or full-speed emulation.

| RSP | Pair | Scalar wall / CPU (s) | AVX2 wall / CPU (s) |
| --- | --- | --- | --- |
| Interpreter | 1 | 7.626 / 7.531 | 7.458 / 7.422 |
| Interpreter | 2 | 7.422 / 7.391 | 7.816 / 7.766 |
| Interpreter | 3 | 7.373 / 7.313 | 7.352 / 7.313 |
| JIT | 1 | 7.588 / 7.547 | 8.965 / 8.781 |
| JIT | 2 | 7.650 / 7.609 | 7.838 / 7.688 |
| JIT | 3 | 7.558 / 7.484 | 7.593 / 7.563 |

Interpreter CPU medians: scalar 7.390625 s, AVX2 7.421875 s (**+0.42%**).
JIT CPU medians: scalar 7.546875 s, AVX2 7.687500 s (**+1.86%**).
The AVX2 candidate is slower in all three JIT pairs. Interpreter pairs include
one improvement, one regression and one CPU tie. This does not establish a
useful improvement; retain the scalar default.

The standalone microbenchmark is only diagnostic: VMADN median lane/broadcast
times were 25.874/26.765 ns scalar and 24.915/26.831 ns AVX2. Other, unchanged
opcodes also varied between batches. Do not use these unpaired numbers to
claim an end-to-end speedup or compare with the previous PC's CSV.

Local artifacts are in `build/vmad-validation/` (preserved scalar/candidate
executables, twelve images and logs, `game-timings.json`, AVX2 Debug test log),
`build/vmad-{before,candidate}.csv`, `build/vmad-{before,candidate}-{120,300,600}.bin`
and `build/measure-vmad.ps1`. ROMs and build outputs remain outside Git.
