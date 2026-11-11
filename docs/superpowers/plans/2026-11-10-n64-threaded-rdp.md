# Threaded N64 RDP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run the existing N64 rasterizer concurrently without changing observable memory, interrupt, or save-state behavior.

**Architecture:** Keep DPC and command assembly on the emulation thread. An ordered worker owns raster state and uses sequence fences with producer-owned 4 KiB memory reservations. Keep synchronous execution as the oracle and default until measured acceptance.

**Tech Stack:** C++20, standard threads/mutex/condition_variable, CMake presets, doctest.

**Spec:** `docs/superpowers/specs/2026-11-10-n64-threaded-rdp-design.md` (approved by the user's instruction to continue).

## Global Constraints

- One worker per console; bounded FIFO initially 1024 packets; maximum packet 22 words.
- RDRAM 8 MiB; reservation page size 4 KiB; complete all conflicting work before main-thread access.
- Save-state version 11 and byte ordering unchanged; no host synchronization state serialized.
- Main thread alone owns scheduler, MI, DPC and partial packet assembly.
- Preserve synchronous default unless median elapsed time improves at least 10% with identical results.
- Build only with CMake presets; never commit ROMs or publish without explicit permission.

## Review Focus

- Self-modifying code/framebuffer alias: CPU fetch and idle-loop reads must obey memory fences (Task 4).
- Partial commands and XBUS reuse: captured words must outlive changes to source memory (Task 3/5).
- Narrow/unaligned framebuffer strides: reservations must include pixels beyond a nominal row width (Task 3).
- Worker exceptions and a saturated queue: every waiter must wake and destruction must join (Task 1).
- Legacy/truncated save states: loading must drain before replacement and preserve rollback (Task 4/5).

### Task 1: Ordered worker and memory fences

**Files:** create `src/core/n64/rdp_worker.hpp`, `rdp_worker.cpp`, `tests/n64/test_rdp_worker.cpp`; update both N64 CMakeLists.

**Interfaces:** `RdpPacket` owns `array<uint64_t,22>` plus word count. `RdpAccess` contains up to two exclusive-end ranges, each read or write. `RdpWorker(callback, capacity=1024)` exposes `submit(packet,access)`, `wait_memory(address,bytes,write)`, `drain()`, `pending()`. Sequence IDs and reservations stay internal.

- [ ] Add deterministic tests using promises/latches for FIFO, conflicting/nonconflicting reads/writes, cross-page ranges, capacity exhaustion and callback exceptions; run and observe RED.
- [ ] Implement the bounded queue and release/acquire completion fences; callbacks cannot call producer APIs; destructor joins without throwing.
- [ ] Run worker tests GREEN; commit independently.

### Task 2: Isolate raster state without changing execution

**Files:** create `src/core/n64/rdp_rasterizer.hpp`; modify `rdp.hpp`, `rdp.cpp`, `rdp_texture.cpp`; create `rdp_commands.cpp`; update CMake.

**Interfaces:** `RdpRasterizer::execute_packet(const RdpPacket&, span<uint8_t>)` executes full non-SyncFull packets. `Rdp` privately derives from rasterizer so existing serialization can preserve exact field order. Raster draw coefficients are separate from the front-end partial-command array.

- [ ] Capture baseline Debug tests and existing 300-field MK64 BMP before extraction.
- [ ] Move drawing methods/state into rasterizer, front-end methods into command interface; synchronous dispatch uses the packet executor. SyncFull stays on main thread.
- [ ] Run all existing N64 tests and exact BMP comparison GREEN; commit.

### Task 3: Connect worker and conservative reservations

**Files:** `rdp.hpp`, `rdp_commands.cpp`, `rdp_worker.hpp/cpp`, tests for command handling.

**Interfaces:** `Rdp::enable_threaded(span<uint8_t>) -> bool`, `drain() const`, `prepare_memory(address,bytes,write) const`, `threaded() const`; worker ownership is a unique_ptr. Access shadow tracks framebuffer address/stride/size, Z address, scissor and modes independently of raster state.

- [ ] Add tests for value-owned packet capture, XBUS, partial END, SyncFull visibility and reservation endpoints; observe RED for threaded activation.
- [ ] Publish ordered state/draw packets; reserve all RAM for texture reads and conservative clipped framebuffer/Z ranges for drawing. Include the last row's X extent even when X exceeds stride. SyncFull drains before MI DP is raised.
- [ ] Add reset/legacy-load helpers that drain and reset data without replacing a running worker; serialization drains and rebuilds access shadow on load.
- [ ] Run tests GREEN; commit.

### Task 4: Protect every memory observer

**Files:** `cpu/cpu_bus.hpp`, `cpu/vr4300.cpp`, `bus.hpp/cpp`, `n64.hpp/cpp`, `tests/n64/test_state.cpp`.

**Interfaces:** CpuBus gains a cheap optional memory-guard flag and virtual guarded-range hook, no-op for flat test buses. `N64::create(rom,pif,rdp_mode)` retains default synchronous mode. Bus owns bounded `image()` extraction instead of publicly exposing raw RDRAM spans.

- [ ] Add CPU instruction-fetch/load/store/idle-loop overlap tests and DMA/VI observers with pending work; observe RED.
- [ ] Guard CPU direct access, bus memory paths, RDP command source reads; drain for PI/SI and SP/AI DMA paths before RAM use. Guard both immediate register-triggered transfers and completion events.
- [ ] Drain before save/load serialization and console destruction; ensure worker dies before RAM and reset recreates selected execution mode.
- [ ] Run tests GREEN; commit.

### Task 5: Differential validation and runner selection

**Files:** `tests/n64/test_main.cpp`, N64 tests, `tools/n64_runner/main.cpp`, frontend only if performance acceptance passes.

**Interfaces:** test executable and runner accept `--rdp-threaded` and `--rdp-sync`; tests select the creation default in their own process so all existing test cases run in both modes. Runner can execute a save/reload continuation check for differential validation.

- [ ] Add cross-mode state, rollback, reset/destruction and queue-pending differential tests; observe RED.
- [ ] Implement mode selection and preserve mode through reload/reset. Register both full N64 suites in CTest.
- [ ] Run Debug CTest for both modes, plus exact Release MK64 images at 120/300/600 fields and save-state continuation equality.
- [ ] Benchmark at least three alternating runs per mode, no parallel builds/tests, recording elapsed and all-thread CPU time. Enable default only if the 10% threshold and all correctness criteria pass.
- [ ] Record missing systemtest ROM and unavailable race-detector coverage accurately; commit.

### Task 6: Whole-change review and delivery

- [ ] Request a fresh technical review of memory ownership, reservation bounds, exception paths and serialization compatibility.
- [ ] Fix actionable findings with regression coverage and rerun affected suites.
- [ ] Update recordatorios and this checklist, commit code/documentation locally on `codex/`; do not publish to main.

## Execution ledger

- Ruling: implement inline in the existing checkout on a dedicated branch, reusing this machine's build tree. The user requested continued execution; no extra worktree or execution-method approval is necessary. Cost: no simultaneous independent checkout; protect the pre-existing untracked ZIP by explicit staging.
- Ruling: the approved design is the implementation authority; finish the plan and execute without another planning approval loop, following the user's instruction to continue.
