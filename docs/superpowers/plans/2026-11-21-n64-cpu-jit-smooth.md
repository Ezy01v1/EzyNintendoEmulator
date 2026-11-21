# CPU JIT Phase 4 (no stutter) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove CPU-JIT compile spikes in MK64: measure per-field times, stop compiling at arbitrary resume PCs, and replace the AsmJit `Compiler` backend with an `Assembler` backend and our own register allocator.

**Architecture:** The runner gains per-field timing and a `--hold` input option to measure spikes. `BlockCache` records which words lie inside a compiled block, and `Vr4300Jit::execute` refuses to compile a covered PC that was not reached by a jump. `ezy_jit` gets an AsmJit-free linear register allocator and a new x86-64 emitter on `x86::Assembler`. The old `Compiler` backend stays selectable while the two are compared, then it is deleted.

**Tech Stack:** C++20, MSVC Build Tools 2022, CMake presets, AsmJit (pinned in `cmake/asmjit.cmake`), doctest.

**Spec:** `docs/superpowers/specs/2026-11-21-n64-cpu-jit-smooth-design.md`

## Global Constraints

- Branch `n64/jit-smooth` (from `n64/vi-output`, PR #18). Commits in English; notes and docs in Spanish, like the existing ones.
- Build only with presets: `cmake --build --preset debug|release`, `ctest --preset debug`. CMake path: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
- The JIT must give exactly the interpreter's result. The reference: MK64 `--dump-state build/vo/now_1900.bin 1900 --audio-hle --rdp-threaded --cpu-jit` has SHA-1 `4a4c0711b75a574a8f50bb2b49934bb6a494d518` today, and it must stay that.
- Goals, verbatim from the spec: no race field above **16.7 ms** (excluding fields whose own compile time is under 2 ms, which are documented as non-CPU spikes); race compile time (1500–1900) **at least 75 % lower** (from ~4 s to ≤ 1 s); mean fields/s not lower (median of ≥ 5 interleaved pairs, ≥ 4 of 5 pairs losing no more than 2 %).
- The allocator and backend live in `ezy_jit` and know nothing of MIPS or N64. Public `ezy_jit` headers never include AsmJit.
- With `EZY_N64_CPU_JIT` OFF everything still builds, and the suites pass.
- MK64: `E:/Rom nintendo/Mario Kart 64 (USA).z64`. Spider-Man: `E:/Rom nintendo/Spider-Man/Spider-Man (USA).z64`. Never commit ROMs or the user's `.ss1` states.
- Python/bash heredocs turn `\n` inside C++ string literals into real newlines, so use the Edit tool for C++ strings.

## Review Focus

1. A loop head inside an earlier block, reached by a taken branch, must still compile and run natively. A refusal there would turn hot loops into interpreted code (Task 2 test "a branch target inside a block compiles").
2. After a page is invalidated or the cache is flushed, a formerly covered PC with new code must compile again, so no stale coverage marks remain (Task 2 tests).
3. An exit reached from several jump sites with different register states must see the right values at each site (Task 3 test "an exit taken from two sites").
4. Values held in volatile registers that are still live after a helper call must survive the call (Task 3 test "values survive a helper call").
5. More live values than host registers must spill and reload correctly in both 32- and 64-bit widths (Task 3 test "spills under pressure").

---

### Task 1: Runner frame timing, `--hold`, and the baseline

**Files:**
- Create: `tools/n64_runner/frame_stats.hpp` (header-only, no N64 dependency)
- Modify: `tools/n64_runner/benchmark.hpp`, `tools/n64_runner/benchmark.cpp`, `tools/n64_runner/main.cpp`
- Test: `tests/n64/test_runner_frame_stats.cpp` (add it to `tests/n64/CMakeLists.txt`, with `${PROJECT_SOURCE_DIR}/tools/n64_runner` as an include directory of the test target)
- Create: `docs/performance/2026-11-21-n64-cpu-jit-smooth.md` (baseline section)

**Interfaces:**
- Produces:
  - `struct FrameSummary { double max_ms, p99_ms, mean_ms; int over_budget; };`
  - `FrameSummary summarize_frames(std::span<const double> ms, double budget_ms);` p99 is exact: sort a copy and take index `ceil(0.99 * n) - 1`. `over_budget` counts values `> budget_ms`.
  - `std::optional<HoldSpec> parse_hold(std::string_view text);` with `struct HoldSpec { std::uint16_t buttons; int from_field; };`. Syntax `NAME[+NAME...][@FIELD]`. The names (case-sensitive) and bits are A 0x8000, B 0x4000, Z 0x2000, Start 0x1000, Up 0x0800, Down 0x0400, Left 0x0200, Right 0x0100, L 0x0020, R 0x0010, CU 0x0008, CD 0x0004, CL 0x0002, CR 0x0001. `from_field` defaults to 0. It returns nullopt on an unknown name, an empty name or a bad field.
  - `benchmark_frames(..., const char* frame_times_path, std::optional<HoldSpec> hold)`. It prints `Frames: {"max_ms":..,"p99_ms":..,"mean_ms":..,"over_16_7":..,"compile_max_ms":..,"compile_total_ms":..}` after the existing lines. `--frame-times <csv>` writes the header `field,ms,compile_ms` and one row per timed field.
  - `--hold` sets `n64.bus().pif().set_buttons(0, hold.buttons, 0, 0)` before every field `>= from_field`. That covers the warm-up fields before `--time-from` too, and `--series`/`--screenshot`/`--dump-state`.

- [ ] **Step 1: Write the failing tests** in `tests/n64/test_runner_frame_stats.cpp`:
  - `TEST_CASE("summarize_frames gives max, exact p99, mean and the count over budget")`: for 100 values `1..100` ms and budget 16.7, expect max 100, p99 99, mean 50.5, over_budget 84.
  - `TEST_CASE("parse_hold reads names, combinations and a start field")`: `"A"` → {0x8000, 0}; `"A+B@1500"` → {0xC000, 1500}; `"CU+R"` → {0x0018, 0}; `"a"`, `""`, `"A@"`, `"A@x"` and `"A+"` → nullopt.
- [ ] **Step 2: Run them to see them fail.** Run `cmake --build --preset debug --target ezy_n64_tests` and expect a compile error (no `frame_stats.hpp`).
- [ ] **Step 3: Implement** `frame_stats.hpp`, then wire `--frame-times` and `--hold` into the runner's option parsing, usage text and run loops. Per-field compile time is the difference of `n64.cpu_jit()->stats().compile_ns` around each `run_frame()`, or 0 without the JIT.
- [ ] **Step 4: Run the tests.** Run `ctest --preset debug -R ezy_n64_tests` and expect PASS.
- [ ] **Step 5: Measure the baseline.** Build Release and copy `build/default/tools/n64_runner/Release/ezy_n64_runner.exe` to `build/baseline/ezy_n64_runner.exe`. Run 3 times:
  `ezy_n64_runner.exe "<MK64>" --benchmark build/vo/b.bmp 1900 --time-from 1500 --audio-hle --rdp-threaded --gfx-hle --cpu-jit --frame-times build/vo/base_frames.csv`
  Record fields/s, `Frames:` and `CPU JIT:` in the doc's "Línea base" section, along with the fields > 16.7 ms and whether their compile_ms explains them.
- [ ] **Step 6: Commit.** Message: `N64 runner: per-field times, --frame-times and --hold; JIT smoothness baseline`

### Task 2: Do not compile at arbitrary resume PCs

**Files:**
- Modify: `src/core/jit/block_cache.hpp`, `src/core/jit/block_cache.cpp`
- Modify: `src/core/n64/cpu/vr4300_jit.hpp`, `src/core/n64/cpu/vr4300_jit.cpp` (`execute`, `Stats`, the runner's `print_cpu_jit_stats` in `tools/n64_runner/main.cpp`)
- Test: `tests/jit/test_block_cache.cpp`, `tests/n64/test_cpu_jit.cpp`

**Interfaces:**
- Produces:
  - `[[nodiscard]] bool BlockCache::covered(std::uint32_t paddr) const;` It is true when some block inserted since the last invalidation of the page, or the last flush, has `block.paddr < paddr < block.paddr + 4 * block.words`. A block's own first word is not covered by it. The lookup is O(1): use per-page marks allocated with the page's slots. `insert` marks, `invalidate` clears the page's marks, and `flush` clears everything. The marks ignore the mode key.
  - `Vr4300Jit::Stats::resume_skips` (std::uint64_t): `execute` calls that stopped because of the rule. The runner prints it as `resume skips:`.
- Rule in `execute`, at the miss: `if (block == nullptr && !cpu_.in_delay_slot_ && cache_.covered(paddr)) { ++stats_.resume_skips; break; }` comes before `compile`.

- [ ] **Step 1: Write the failing tests.**
  - In `tests/jit/test_block_cache.cpp`:
    - `TEST_CASE("covered marks a block's words after its first")`: insert a 4-word block at 0x1000; `covered(0x1000)` is false, `0x1004`/`0x100C` are true, `0x1010` is false.
    - `TEST_CASE("invalidate and flush clear coverage")`: after `invalidate(0x1000, 0x1004)` (same page), `covered(0x1004)` is false. Insert again, `flush()`, and it is false again.
  - In `tests/n64/test_cpu_jit.cpp` (use the existing JIT test helpers):
    - `TEST_CASE("an arbitrary resume inside a compiled block does not compile")`: run a straight-line block once. Then set the PC to its third instruction with `in_delay_slot_` false and call `execute`. `stats().compiled` is unchanged, `resume_skips` grows by 1, and `cycles` is 0.
    - `TEST_CASE("a branch target inside a block compiles")`: code where a block falls into a loop whose head is the block's third word and a backward branch targets it. After enough iterations, `compiled` counts a block at the loop head and the loop instructions are counted as `native`.
    - `TEST_CASE("the resume rule keeps the interpreter's result")`: use the existing twin/differential harness on a generated program with interrupts or budgets that force mid-block resumes. The states are identical.
- [ ] **Step 2: Run them to see them fail** (`covered` and `resume_skips` don't exist yet). Run `cmake --build --preset debug` and expect compile errors.
- [ ] **Step 3: Implement** `covered()` and the rule.
- [ ] **Step 4: Run the tests and the full checks.** `ctest --preset debug` gives 11/11. Release `ctest --test-dir build/default -C Release -R "peterlemon|systemtest"` gives 183/183. The MK64 dump-state SHA-1 is `4a4c0711...`.
- [ ] **Step 5: Measure.** Run 5 interleaved pairs of `build/baseline/ezy_n64_runner.exe` against the new Release runner with the Task 1 command, and add a "Regla de entrada" section to the doc: fields/s, `Frames:`, compiled blocks, race compile ms (the field-1900 total minus a run to 1500) and resume skips.
- [ ] **Step 6: Commit.** Message: `N64 JIT: do not compile at arbitrary resume PCs inside compiled blocks`

### Task 3: Linear register allocator and the Assembler backend

**Files:**
- Create: `src/core/jit/reg_alloc.hpp`, `src/core/jit/reg_alloc.cpp` (no AsmJit)
- Create: `src/core/jit/x64_assembler.cpp` (the new emitter; AsmJit only inside `#if EZY_JIT_AVAILABLE`)
- Modify: `src/core/jit/x64_backend.hpp`, `src/core/jit/x64_backend.cpp` (backend choice), `src/core/jit/CMakeLists.txt`
- Test: `tests/jit/test_reg_alloc.cpp` (new, add it to `tests/jit/CMakeLists.txt`), `tests/jit/test_backend.cpp`

**Interfaces:**
- Produces:
  - `enum class Backend : std::uint8_t { Assembler, Compiler };`
  - `[[nodiscard]] Compiled compile(const IrBlock& block, CodeBuffer& buffer, Backend backend = Backend::Assembler);`
  - `reg_alloc.hpp`, for the emitter and the tests:
    - `std::vector<std::uint32_t> next_use_positions(const IrBlock&)`: per body op, the next body index where each operand is used (or `kNever`). The builder may shape this differently, as long as the eviction tests below can be written against it.
    - `class RegAlloc` over abstract register indices `0..N-1`, built with `RegAlloc(unsigned registers, std::uint32_t volatile_mask)`. It has `Location get(std::uint32_t value, std::uint32_t position)` (a register, loading from a spill slot if needed), `Location define(std::uint32_t value, std::uint32_t position)`, `void release_dead(std::uint32_t position)`, `std::vector<Move> before_call(std::uint32_t position)` (it spills or moves the live values in volatile registers), `Snapshot snapshot() const` and `void restore(const Snapshot&)`. `Move` and `Location` say register↔slot transfers. On eviction the value with the farthest next use leaves first.
- Host registers, fixed by this plan:
  - Pinned: `r15` = ctx, `r14` = guest base, `r13` = RAM base (loaded in the prologue only if used).
  - Allocatable, in order: `rbx, rsi, rdi, rbp, r12, r8, r9, r10, r11`. The last four are volatile.
  - Scratch, never allocated: `rax, rcx, rdx` (shifts by `cl`, `mul`/`imul` into `rdx:rax`, call arguments and the return value).
- Calls follow Windows x64: arguments in `rcx, rdx, r8, r9`, return in `rax`, 32-byte shadow space, `rsp` 16-byte aligned at the call. The prologue pushes only the callee-saved registers the block uses, then reserves `8 * spill slots + 32`, rounded up for alignment.
- Exits: each jump site (`ExitIf`, `Barrier` slow path) gets its own copy of the exit's ops, emitted after the body with the allocator snapshot taken at the site. `Exit` (the body's end) emits its exit inline.

- [ ] **Step 1: Write the failing allocator tests** in `tests/jit/test_reg_alloc.cpp`:
  - `TEST_CASE("eviction spills the value used farthest away")`: with 2 registers and values used at known positions, the third definition spills the one with the later next use.
  - `TEST_CASE("before_call keeps only live volatile values safe")`: a value in a volatile register used after the call gets a Move; a dead one doesn't.
  - `TEST_CASE("snapshot and restore give the state of a jump site")`.
- [ ] **Step 2: Write the failing backend tests** in `tests/jit/test_backend.cpp`:
  - Run every existing native `TEST_CASE` body through both backends. Turn each into a helper taking a `Backend` and call it twice (`SUBCASE("Assembler")`/`SUBCASE("Compiler")`).
  - `TEST_CASE("spills under pressure")`: load 24 guest I64 values and 8 I32 values, combine them pairwise in an order that keeps all of them live, and store every result. Both backends give the same guest memory, equal to a C++ reference.
  - `TEST_CASE("values survive a helper call")`: live values defined before a `call` are used after it.
  - `TEST_CASE("an exit taken from two sites")`: two `exit_if` to the same exit, with different values in registers at each site; the exit's ops store a value defined before both sites, and both paths give the right store.
  - `TEST_CASE("random IR blocks match across backends")`: 500 blocks generated from a fixed seed (ALU ops on I32/I64, compares, selects, guest loads and stores, a helper call, exit_if). Both backends give identical guest memory and exit ids.
- [ ] **Step 3: Run them to see them fail.** Run `cmake --build --preset debug --target ezy_jit_tests` and expect compile errors.
- [ ] **Step 4: Implement** `reg_alloc`, `x64_assembler.cpp` and the `Backend` parameter. The MIPS front-end keeps calling `jit::compile(ir, cache_.code())`, which now means `Assembler`.
- [ ] **Step 5: Run the tests and the full checks.** `ctest --preset debug -R ezy_jit_tests` passes, then the full Debug suite 11/11, Release 183/183 (`--cpu-jit-check` included in the CTest Release entries), the MK64 dump-state SHA-1 `4a4c0711...`, and Spider-Man `--dump-state 1200 --audio-hle --rdp-threaded --cpu-jit` equal with both backends. For that last check, set a temporary environment override of the backend in the runner only, not committed, or a one-off local build.
- [ ] **Step 6: Commit.** Message: `JIT: linear register allocator and an x86::Assembler backend (Compiler kept for comparison)`

### Task 4: Measure, remove the Compiler backend, close the phase

**Files:**
- Modify: `src/core/jit/x64_backend.cpp` and `.hpp` (delete the `Compiler` emitter and the `Backend` enum), `tests/jit/test_backend.cpp` (drop the Compiler subcases; keep the tests themselves)
- Modify: `docs/performance/2026-11-21-n64-cpu-jit-smooth.md`, `pendientes/recordatorios.md`

**Interfaces:**
- Consumes: Task 1's `Frames:` output, Task 2's `resume_skips`, Task 3's backend.
- Produces: `compile(const IrBlock&, CodeBuffer&)` with the original signature, backed by the Assembler emitter only.

- [ ] **Step 1: Measure** with 5 interleaved pairs against `build/baseline/ezy_n64_runner.exe` (Task 1 command), plus one `--profile`. Add a "Backend nuevo" section to the doc: fields/s per pair and median, `Frames:`, race compile ms, compile time per block (total compile ms / compiled), and the profile share of AsmJit. Judge each spec goal (1, 2, 3) as met or not met. If the user has saved a DK's Jungle Parkway state by now, also run it with `--load-state "E:/Rom nintendo/Mario Kart 64 (USA).ss1" --hold A` for 600 fields, before and after, and report its max field and its over-16.7 count.
- [ ] **Step 2: Delete the Compiler backend** and its subcases. Run `cmake --build --preset debug` and `ctest --preset debug` and expect 11/11. Then run Release 183/183 and confirm the MK64 SHA-1 again.
- [ ] **Step 3: Close the phase in the doc.** If goals 1–2 are not met, write the remaining profile and propose the next round (background compilation or other) without adding optimizations. Update `pendientes/recordatorios.md` with the result line, the next steps (user tests DK's bridge and the second-lap question, then the PR for `n64/jit-smooth`) and a log entry.
- [ ] **Step 4: Commit.** Message: `JIT: drop the Compiler backend; phase 4 results`
