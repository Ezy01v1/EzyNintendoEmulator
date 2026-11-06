# Game Boy Accuracy Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the scanline PPU with a dot-accurate pixel-FIFO PPU, and make OAM DMA, the TIMA reload and CPU memory-access timing cycle-accurate. Mooneye's timing ROMs must pass with no regressions.

**Architecture:** Each change is local to its component (`timer`, `bus` OAM DMA, `cpu`), except the PPU. `ppu.cpp` keeps its public interface (`tick(int dots)`, register access, `framebuffer()`, locks, `take_*` signals). Internally it becomes a dot-stepped state machine with an OAM scan, a BG/window fetcher, a BG FIFO and an OBJ FIFO. The FIFO lives in new `ppu_fifo.hpp/.cpp` so `ppu.cpp` stays readable.

**Tech Stack:** C++20, CMake presets (`build/default`), doctest, Mooneye Test Suite in `roms/mooneye/`.

**Spec:** `docs/superpowers/specs/2026-11-06-gb-accuracy-design.md`

## Global Constraints

- Build with the presets (`cmake --build --preset release`). Run the ROM tests with `ctest --test-dir build/default -C Release`.
- English code, comments and commits. Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Follow the style of `src/core/gb`.
- The goal is to match the hardware, not to force a ROM to pass. Mooneye ROMs without a model suffix are required with `--model dmg`. `-C` ROMs are required with `--model cgb`. Everything else on CGB is reported but does not block.
- There is one PPU with branches per model. The scanline renderer is deleted.
- Hard gate: real speed (59.7 FPS) on this PC. Soft goal: < 25 % of one core. Start simple and optimise only if the measurement asks for it.
- Save states: bump `kStateVersion` to 2, serialize all new state, and reject old states.
- Double speed: the PPU gets 2 dots per CPU M-cycle. OAM DMA and the timer run on the CPU clock. CGB-only quirks (OAM DMA + HDMA together, CGB OAM-scan oddities) are out of scope.
- No regressions on: cgb-acid2, dmg-acid2 (both models), Blargg cpu_instrs and instr_timing (both models), boot_regs-*, every Mooneye ROM in the Task 0 baseline, and the GB and GBA unit tests.

## Review Focus

1. A game that writes VRAM or OAM while the CPU is locked out must see `0xFF` / dropped writes at the exact dots the FIFO uses them. This is the most likely source of graphical glitches in commercial games. Test in Task 4: `"VRAM lock follows the real mode 3 end"`.
2. Turning the LCD off mid-frame and on again must restart cleanly: line 0, no stale FIFO pixels, and the first line has no OAM scan (mode 0 instead of mode 2). Test in Task 6.
3. A save state taken in the middle of mode 3 must resume and produce an identical frame. Test in Task 7.
4. OAM DMA started from source `0xFE00-0xFFFF` must read from the echo of `0xDE00+` like the hardware, and must not crash. Test in Task 2.
5. Ten or more sprites on one line, sprites at X = 0 / partly off-screen, and sprites with X ≥ 168 must follow the 10-per-line limit and the fetch stalls without hanging mode 3. Test in Task 5.

---

### Task 0: Baseline (Mooneye lists and performance)

**Files:** Modify `roms/README.md`.

- [ ] **Step 1:** On the current branch, before any code change, run every `.gb` under `roms/mooneye/{acceptance,emulator-only,misc}` with `ezy_gb_runner <rom> --mooneye 10 --model dmg`, and again with `--model cgb`. Write two sorted lists, `Passed`/`Failed` per ROM, into `roms/README.md` under "Mooneye baseline before the accuracy pass".
- [ ] **Step 2:** Time `ezy_gb_runner roms/cgb-acid2.gbc --screenshot NUL 3600` and `ezy_gb_runner roms/pokemon-silver.gbc --screenshot NUL 3600` (Release, 3 runs each, take the median). Record the seconds and `seconds / 60.3` (the fraction of one core used at real speed) in the same section.
- [ ] **Step 3: Commit** `GB: record the Mooneye and performance baseline`

### Task 1: TIMA reload delay

**Files:** Modify `src/core/gb/timer.hpp/.cpp`. Test: `tests/gb/test_timer.cpp`.

**Interfaces:** Produces new private state `reload_delay_` (`int`, M-cycles until TMA is loaded), which is serialized.

Behaviour (Pan Docs "Timer obscure behaviour"):
- On overflow, TIMA reads `0x00` for one M-cycle. Then it is loaded from TMA and IF bit 2 is set.
- A write to TIMA during that cycle cancels both the reload and the interrupt.
- A write to TIMA on the reload cycle is ignored.
- A write to TMA on the reload cycle is loaded into TIMA too.

- [ ] **Step 1: Write failing tests:**
  - `"TIMA reads 0 for one cycle after overflow"`
  - `"writing TIMA during the delay cancels the reload and interrupt"`
  - `"writing TIMA on the reload cycle is ignored"`
  - `"writing TMA on the reload cycle also reaches TIMA"`
- [ ] **Step 2: Run** `build/default/tests/gb/Release/ezy_gb_tests.exe -tc="*TIMA*,*TMA*"`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all GB unit tests and `ezy_gb_runner roms/mooneye/acceptance/timer/<each>.gb --mooneye --model dmg`. Expected: all of `acceptance/timer/*` pass, and Blargg `cpu_instrs` and `instr_timing` still pass.
- [ ] **Step 5: Commit** `GB: TIMA reload delay`

### Task 2: OAM DMA that takes 160 M-cycles

**Files:** Modify `src/core/gb/bus.hpp/.cpp`. Test: `tests/gb/test_bus.cpp`.

**Interfaces:** Produces `Bus::oam_dma_active() const`. New serialized state: `dma_source_` (`std::uint16_t`), `dma_index_` (0-160), `dma_start_delay_` (M-cycles before the first byte, 1 after the write; a restart keeps the old transfer running during the new one's delay).

Behaviour:
- One byte per M-cycle, driven from `advance()`.
- While active, CPU reads outside HRAM (`0xFF80-0xFFFE`) return the byte currently being transferred, and CPU writes outside HRAM are dropped. I/O stays reachable; check `oam_dma_*` in Mooneye for the exact rule.
- OAM reads during DMA return `0xFF`.
- Source pages `0xFE`/`0xFF` read from `0xDE00`/`0xDF00`, the echo of WRAM (Review Focus 4).

- [ ] **Step 1: Write failing tests:**
  - `"OAM DMA copies one byte per M-cycle"`: after the write plus 1 delay cycle plus 80 ticks, exactly 80 bytes are copied.
  - `"CPU reads outside HRAM see the DMA byte during DMA"`
  - `"HRAM stays readable during DMA"`
  - `"restarting DMA mid-transfer"`
  - `"DMA from 0xFE00 reads the WRAM echo"`
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the unit tests and Mooneye `oam_dma/*`, `oam_dma_start`, `oam_dma_restart`, `oam_dma_timing` (DMG). Expected: PASS, and dmg-acid2 and cgb-acid2 still match.
- [ ] **Step 5: Commit** `GB: OAM DMA takes 160 M-cycles and owns the bus`

### Task 3: CPU memory-access timing and ie_push

**Files:** Modify `src/core/gb/cpu.cpp`. Test: `tests/gb/test_cpu.cpp`.

Mooneye's `*_timing` ROMs use OAM DMA to see *when* each access happens, so first re-run them after Task 2. Only fix what still fails. Reference: *Game Boy: Complete Technical Reference* (gekkio) instruction timing tables.

- [ ] **Step 1: Run** `call_timing`, `call_timing2`, `call_cc_timing`, `call_cc_timing2`, `ret_timing`, `ret_cc_timing`, `reti_timing`, `jp_timing`, `jp_cc_timing`, `rst_timing`, `push_timing`, `add_sp_e_timing`, `ld_hl_sp_e_timing` and `interrupts/ie_push` with `--model dmg`. List the failures in the ledger.
- [ ] **Step 2: Write a failing test for each instruction that still fails.** Use the `FlatBus` access log in `tests/gb/flat_bus.hpp`, adding an `std::vector<Access>` log if it has none. Each test asserts the exact sequence of read/write/internal cycles. Example: `"CALL nn: read lo, read hi, internal, write PC hi to SP-1, write PC lo to SP-2"`. For `ie_push`: `"interrupt dispatch re-reads IE after pushing PC high byte"`. If the push overwrote IE so that nothing is pending any more, PC becomes `0x0000`.
- [ ] **Step 3: Run.** Expected: FAIL.
- [ ] **Step 4: Implement** by reordering the `bus_.read/write/tick` calls in `cpu.cpp`.
- [ ] **Step 5: Run** the unit tests, SingleStepTests (`ctest -C Release -R gb_single_step`), Blargg, and the Mooneye list above. Expected: all pass.
- [ ] **Step 6: Commit** `GB: exact memory access order inside instructions`

### Task 4: FIFO PPU, background and window

**Files:**
- Create `src/core/gb/ppu_fifo.hpp/.cpp`, and add them to `src/core/gb/CMakeLists.txt`.
- Modify `src/core/gb/ppu.hpp/.cpp`: delete `render_scanline`, `mode3_length`, `hblank_start_` and the BG half of the renderer.
- Test: `tests/gb/test_ppu.cpp`.

**Interfaces:**
- Produces `struct BgPixel { std::uint8_t color; std::uint8_t palette; bool priority; };` and `struct ObjPixel { std::uint8_t color; std::uint8_t palette; bool bg_priority; std::uint8_t oam_index; };`.
- Produces `template <class T> class PixelFifo` (capacity 16, with `push8`, `pop`, `size`, `clear`) and `class Fetcher` (a 6-dot fetch: tile number, data low, data high, push, each step 2 dots). They live in `ppu_fifo.hpp`.
- `Ppu` keeps every public member it has today. Mode 3 ends when the LCD X counter reaches 160. `mode()` and the locks follow it.

Behaviour (Pan Docs "Pixel FIFO"; *The Ultimate Game Boy Talk* timing):
- Mode 3 starts with a dummy fetch.
- `SCX % 8` pixels are discarded at the start of each line.
- The window triggers when `WX - 7 == x` and `WY` has matched on this frame. The fetcher then restarts with the window map and the window line counter.
- CGB attributes (bank, flips, palette, priority) apply per fetched tile.
- `BGP`, `SCX` (low bits only at line start, high bits per fetch), `LCDC` and the CGB palette RAM are read when the hardware reads them. Mid-line writes show up.

- [ ] **Step 1: Write failing tests:**
  - `"mode 3 lasts 172 dots with SCX = 0 and no sprites"`
  - `"SCX % 8 adds its value to mode 3"`
  - `"window adds 6 dots"`
  - `"a mid-line BGP write changes only the pixels after it"`: tick to x ≈ 80, write BGP, then check pixels 0-79 against pixels 88-159.
  - `"VRAM lock follows the real mode 3 end"` (Review Focus 1)
- [ ] **Step 2: Run.** Expected: the mid-line test FAILS.
- [ ] **Step 3: Implement.** Sprites are disabled in this task, with a TODO pointing to Task 5.
- [ ] **Step 4: Run** the unit tests, plus cgb-acid2 and dmg-acid2 with `--compare`. acid2 draws sprites, so it may fail until Task 5; note that in the ledger. Run Blargg too.
- [ ] **Step 5: Commit** `GB: pixel FIFO for background and window`

### Task 5: FIFO PPU, sprites

**Files:** Modify `src/core/gb/ppu.cpp`, `ppu_fifo.hpp/.cpp`. Test: `tests/gb/test_ppu.cpp`.

Behaviour:
- Mode 2 scans OAM over 80 dots and keeps up to 10 sprites (2 dots per entry), in OAM order.
- When the X counter reaches a sprite's X, the BG fetcher pauses and the sprite tile is fetched. The stall is 6 dots, plus up to 5 more while the BG fetch finishes.
- Sprite pixels mix into the OBJ FIFO. On DMG / with OPRI = 1 an existing opaque pixel is never replaced (lower X wins). On CGB the lower OAM index wins.
- Sprites with X < 8 are clipped. X ≥ 168 is never shown, but still counts toward the 10 (Review Focus 5).
- Final mix: the existing DMG/CGB priority rules.

- [ ] **Step 1: Write failing tests:**
  - `"one sprite at X=8 adds 11 dots"`
  - `"sprite at X=0 is hidden but stalls"`
  - `"11 sprites on a line: only 10 drawn"`
  - `"sprites at X >= 168 count but don't draw"`
  - `"DMG overlap: lower X wins; CGB: lower OAM index wins"`
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the unit tests and all ctest ROM tests. cgb-acid2 and dmg-acid2 (both models) must match exactly. Then run Mooneye `intr_2_mode0_timing_sprites` and `intr_2_mode3_timing` (DMG).
- [ ] **Step 5: Commit** `GB: sprites in the pixel FIFO`

### Task 6: Dot-exact STAT, LY and LCD-on timing

**Files:** Modify `src/core/gb/ppu.cpp`. Test: `tests/gb/test_ppu.cpp`.

Behaviour (Mooneye sources next to each ROM, *Cycle-Accurate GB Docs*):
- LY increments at dot 0 of the line.
- On line 153, LY reads 153 for only a few dots, then 0. LY=LYC compares against that.
- The STAT IRQ line is an OR of its sources, and only a rising edge requests the interrupt (blocking).
- The mode 2 interrupt fires on line 0 and after VBlank.
- When the LCD turns on, line 0 starts with no OAM scan: mode 0 for 80 dots, and LY=LYC is checked right away.
- Turning the LCD off clears the FIFOs (Review Focus 2).

- [ ] **Step 1: Write failing tests:**
  - `"LY reads 0 early on line 153"`
  - `"LCD on: first line has no mode 2"`
  - `"LCD off and on mid-frame restarts at line 0 with empty FIFOs"`
  - `"STAT IRQ blocking"`
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** every required Mooneye ROM from the spec, plus the Task 0 baseline lists (no `Passed` may become `Failed`). Run all ctest.
- [ ] **Step 5: Commit** `GB: dot-exact STAT, LY and LCD-on timing`

### Task 7: Save states v2, performance, test registration

**Files:** Modify `src/core/gb/gameboy.cpp` (`kStateVersion = 2`), every new `serialize()`, `tests/gb/test_state.cpp`, `tests/gb/CMakeLists.txt` and `roms/README.md`.

- [ ] **Step 1: Write failing tests:**
  - `"state saved in mode 3 resumes to an identical frame"` (Review Focus 3): stop with `ppu().mode()==3` and check that the frames match.
  - `"version 1 states are rejected"`
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Make sure the FIFOs, fetcher, OAM-scan list, DMA and timer delay are all serialized.
- [ ] **Step 4:** Register every required Mooneye ROM that passes as an `ezy_gb_rom_test(... --mooneye --model dmg|cgb)` in `tests/gb/CMakeLists.txt`. Run `ctest --test-dir build/default -C Release`. Expected: 100 %.
- [ ] **Step 5:** Re-run the Task 0 performance measurement and the full Mooneye lists. In `roms/README.md`, write the before/after times and core fraction, and the new known-failures list. If the hard gate (60 s of frames in < 60 s) fails, stop and report. If the soft goal (< 25 %) fails, report it with a proposed optimisation.
- [ ] **Step 6: Commit** `GB: save state v2, accuracy ROM tests and measurements`
