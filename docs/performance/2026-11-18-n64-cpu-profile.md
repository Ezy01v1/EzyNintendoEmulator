# N64: CPU profile of the MK64 race before the JIT (2026-11-18)

## Command

```
build/default/tools/n64_runner/Release/ezy_n64_runner.exe "E:/Rom nintendo/Mario Kart 64 (USA).z64" \
  --benchmark build/race.bmp 1900 --time-from 1500 --audio-hle --rdp-threaded --gfx-hle \
  --profile build/cpu-profile.csv
```

Fields 1500-1900 (400 fields), Release, RSP interpreter. The profiled run did
49.8 fields/s (8.03 s wall, 14.7 s process CPU). Profiled timings are not a
performance result; they are only the baseline for this profile. The sampler only
sees the main (emulation) thread: 3996 samples, 0 capture errors.

## Main-thread share by function group

| Group | Functions | Share |
|---|---|---|
| Interpreter dispatch and run loop | `N64::step` (fetch, scheduler and CPU loop inlined by LTO) | 31.7 % |
| Instruction decode and execute | `Vr4300::execute` 16.0, `execute_special` 4.2, `execute_load_store` 4.4 | 24.7 % |
| Memory access | `load<u32>` 11.4, `load<u16/u8/u64>` 1.6, `store<*>` 1.3 | 14.3 % |
| COP1 (FPU) | `execute_cop1` 2.7, its lambdas 1.0 | 3.7 % |
| FP environment and libm | `fesetround`, `fegetenv`, `truncf`, `fdclass`, `fdtest`, `fpclassify`, `feclearexcept`, `fetestexcept`, `trunc`, ... | 6.1 % |
| Idle-loop skip | `Vr4300::idle_loop_cycles` | 3.4 % |
| Bus and scheduler | `Bus::interrupt_line` 1.6, `Bus::prepare_memory` 1.0, `Scheduler`/`Bus::*` < 0.2 | 2.8 % |
| RDP sync | `Rdp::prepare_memory` (waiting for the RDP worker) | 7.7 % |
| Audio HLE | `Nead::*` | 1.5 % |
| Graphics HLE | `F3dex::*` | ~0.8 % |
| RSP | interpreter / native code | not separated (< 1 %) |
| Other | `atof` 1.3 (probably an identical-code-folding alias), `main`, ntdll | ~3 % |

The VR4300 interpreter (first five rows plus idle loop, Bus and the FP library
calls) is about 86 % of the main thread, so CPU work is the bottleneck this JIT
targets. The FP environment calls (`fesetround`/`fegetenv`/`fdclass`...) are 6 %
by themselves and are worth avoiding in the JIT's COP1 path.

## TLB check

`Vr4300::translate_mapped` and `Tlb::translate` (and anything else TLB-related)
have **0 samples** in the race (0.0 %, threshold 2 %). Both are real out-of-line
functions in the sources and absent from the CSV, so they were not inlined away.
The 11.4 % in `load<u32>` is the access path, mostly unmapped KSEG0/KSEG1
addresses.

Consequence: the threshold is not met, so Phase 5 (TLB page table) does not
enter the project. Revisit only if another game shows TLB time.
