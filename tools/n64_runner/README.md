# N64 race measurements

Build through the repository presets. Use a local, legally obtained ROM; ROMs,
screenshots and saved states belong in ignored `build/`, never in Git.

```powershell
cmake --build --preset release --target ezy_n64_runner --parallel 1
build/default/tools/n64_runner/Release/ezy_n64_runner.exe <rom> --rsp-interpreter --rdp-sync --benchmark build/race.bmp 1900 --time-from 1500 --dump-final-state build/race.bin
```

The runner first executes 1500 VI fields without timing. It drains the RDP, then
times the next 400 fields and a final drain, reporting one `Benchmark:` JSON
record. Image encoding, state serialization, output and symbol resolution are
excluded. Fields are video interrupts, not necessarily newly drawn game frames.
CPU seconds count the whole process, including a threaded RDP when selected.
Use the same ROM, endpoints, backend options and Release build for comparisons.

For three alternating cold-process pairs of interpreter/JIT, with exact image
and complete-state comparison after every process:

```powershell
tools/n64_runner/benchmark-race.ps1 -Rom <rom> -OutputDirectory build/race-backends
```

To compare a preserved baseline executable with a candidate using the same RSP:

```powershell
tools/n64_runner/benchmark-race.ps1 -Rom <rom> -BaselineRunner <baseline.exe> -Backend interpreter -OutputDirectory build/race-candidate
```

`-Runner` overrides the candidate executable; `-First`, `-End` and `-Pairs`
override the default 1500/1900/3. Each process boots independently; the warm-up
still runs outside the measurement window to warm its JIT translations. Do not
run builds or other emulator workloads simultaneously. Keep adjacent paired
CPU/wall results as well as medians, and distinguish evidence from noisy runs.
Use `-Reverse` with a separate output directory for a complementary batch that
starts with candidate/baseline instead of baseline/candidate.

## Optional statistical profile (Windows x64)

Add `--profile build/race-profile.csv` to the benchmark command. A helper thread
briefly suspends the emulation thread, captures RIP and resumes it before any
allocation, container access or symbol work. It requests 1 ms timer resolution
only during sampling and restores the request on stop. Actual sample intervals
depend on scheduling; the CSV reports counts, not an assumed sampling rate.

After the measured loop, the sampler joins and DbgHelp resolves the local PDB on
one thread. Retain matching executable/PDB files. Private executable regions in
this runner are classified as RSP native code while the process is still live.
The tool samples only the main thread: use synchronous RDP for CPU/RSP/RDP
attribution. It does not profile the RDP worker or provide inclusive call stacks.
LTO inlining and identical-function folding can attribute shared instruction
addresses to a surviving alias, including library template names. Treat category
totals as approximate and confirm hypotheses with unprofiled comparisons.

**Do not use profiled timings as performance results.** Compare the profiled
image/state with an unprofiled run, check capture errors and sample coverage,
and collect speed measurements separately without `--profile`.

API constraints: [GetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadcontext)
requires a suspended target thread;
[SymFromAddr](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symfromaddr)
and other DbgHelp calls require single-threaded access.
