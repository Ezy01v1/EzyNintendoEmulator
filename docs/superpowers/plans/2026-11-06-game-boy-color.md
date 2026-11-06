# Game Boy Color Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run DMG and CGB games on emulated CGB-E hardware (or a forced DMG), with colour correction, GB save states and CGB debugger views.

**Architecture:** One `ezy::gb` core with a `Model` (`Dmg`, `Cgb`) chosen at creation, plus a `cgb_mode` flag. `cgb_mode` is false when a DMG game runs on a CGB (compatibility mode). The PPU always outputs RGB555, and the frontend does the colour correction. The CPU-speed clock (`Bus::advance`) and the dot clock are split so that double speed only speeds up CPU, timer, serial and OAM DMA.

**Tech Stack:** C++20, CMake presets (`build/default`), doctest, SDL2 + Dear ImGui frontend.

**Spec:** `docs/superpowers/specs/2026-11-06-game-boy-color-design.md`

## Global Constraints

- Build only with presets: `cmake --preset default`, `cmake --build --preset debug` / `--preset release`, `ctest --preset debug`.
- Code, comments and commit messages are in English. Each commit ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Follow the existing style of `src/core/gb`: C++20, `[[nodiscard]]`, `std::span`, comments that explain the hardware.
- Each bus access costs 1 M-cycle and advances everything else. In double speed, 1 CPU M-cycle = 2 PPU dots, not 4.
- Simulated hardware revision: **CGB-E**. `--model auto` (the default) means CGB for every ROM.
- No pixel-FIFO PPU. The mode-3 length is variable and approximate. Dot-exact Mooneye failures are acceptable.
- DMG compatibility palettes follow the real boot ROM. With no match, the boot ROM's default palette is used. There is no colour fallback.
- Must not regress: `cpu_instrs`, `dmg-acid2`, `rtc3test` and every existing unit test, with `--model dmg`.
- Required to finish: `cgb-acid2`, `dmg-acid2` (DMG and CGB-compat), Blargg `cpu_instrs` + `instr_timing` on CGB, Mooneye `boot_regs-cgb`, and all unit tests.
- Commercial ROMs are never committed (`roms/` is git-ignored).

## Review Focus

1. A DMG-only ROM (`0x0143` = `0x00`) under `--model auto` must look coloured (compat palette) and must not see CGB registers: `VBK`/`SVBK`/palette writes are ignored and reads return `0xFF`. Test in Task 2.
2. `SVBK` = 0 must select bank 1, never bank 0 at `0xD000`. Test in Task 2.
3. An HDMA started while the LCD is off must not hang the game. Real hardware transfers nothing until the LCD turns on again, and writing `0xFF55` bit 7 = 0 cancels it. Test in Task 6.
4. Loading a GB save state of another ROM, of another model, or of an older format version must return false and leave the running machine untouched. Test in Task 11.
5. `STOP` without a speed switch armed (`KEY1` bit 0 = 0) must not switch speed, and it must reset DIV like the hardware does. Test in Task 7.

---

### Task 1: Model selection and CGB-E post-boot state

**Files:**
- Create: `src/core/gb/model.hpp`
- Modify: `src/core/gb/cartridge.hpp/.cpp` (header flag), `src/core/gb/gameboy.hpp/.cpp`, `src/core/gb/cpu.hpp/.cpp` (`reset_post_boot`), `src/core/gb/bus.hpp`, `src/core/gb/ppu.hpp`, `src/core/gb/timer.hpp`
- Test: `tests/gb/test_cgb.cpp` (new, add to `tests/gb/CMakeLists.txt`)

**Interfaces:**
- Produces: `enum class Model : std::uint8_t { Dmg, Cgb };` `enum class ModelChoice { Auto, Dmg, Cgb };` `struct Options { ModelChoice model = ModelChoice::Auto; std::optional<std::vector<std::uint8_t>> boot_rom; };` in `model.hpp`.
- Produces: `CartridgeHeader::cgb_flag` (`std::uint8_t`, byte `0x0143`), `CartridgeHeader::licensee_new` / `licensee_old`, used by Task 9.
- Produces: `GameBoy::create(std::span<const std::uint8_t> rom, Options options = {})`, `GameBoy::model()`, `Bus::model()`, `Bus::cgb_mode()` (true only when model is Cgb and the header flag is `0x80`/`0xC0`).
- Produces: `Cpu::reset_post_boot(Model model, bool cgb_mode)`.

- [ ] **Step 1: Write failing tests** `TEST_CASE("auto model runs every ROM as CGB")` (DMG ROM → `model()==Model::Cgb`, `cgb_mode()==false`; flag `0x80` → `cgb_mode()==true`), and `TEST_CASE("CGB-E post-boot registers")`: for a CGB game, `AF=0x1180 BC=0x0000 DE=0xFF56 HL=0x000D SP=0xFFFE PC=0x0100`. For a DMG game on a CGB, `B` is the title-checksum-dependent value from Pan Docs "Power Up Sequence" (`B=0x43` for the default `make_rom()`; check it against Pan Docs). With `--model dmg` the registers stay as they are today. Check the I/O post-boot values from the Pan Docs CGB column, e.g. `LCDC=0x91`, `STAT` bits, `DIV` (the CGB value, not `0xAB`).
- [ ] **Step 2: Run** `ctest --preset debug -R ezy_gb_tests`. Expected: build FAIL (`Model` undefined).
- [ ] **Step 3: Implement.** Thread `Model`/`cgb_mode` from `GameBoy::create` into `Bus`, `Ppu`, `Timer` constructors. Take the post-boot values from Pan Docs (the CGB column, CGB-E).
- [ ] **Step 4: Run** the unit tests plus `ezy_gb_runner roms/cpu_instrs.gb` and the dmg-acid2 screenshot with `--model dmg` (the flag comes in Task 12; until then the default is DMG in the runner). Expected: all pass.
- [ ] **Step 5: Commit** `GB: hardware model selection and CGB-E post-boot state`

### Task 2: VRAM/WRAM banking and the small CGB registers

**Files:** Modify `src/core/gb/bus.hpp/.cpp`, `src/core/gb/ppu.hpp/.cpp`. Test: `tests/gb/test_cgb.cpp`.

**Interfaces:**
- Produces: `Ppu` VRAM becomes `std::array<std::uint8_t, 0x4000>`. Add `Ppu::vram_bank()`, and `Ppu::read_vram_bank(int bank, std::uint16_t addr) const` for the debugger and HDMA. WRAM becomes `std::array<std::uint8_t, 0x8000>`, with `Bus::wram_bank()`.
- Registers: `VBK 0xFF4F` (bit 0, reads `0xFE|bank`). `SVBK 0xFF70` (bits 0-2, 0→1, reads `0xF8|v`). `KEY0 0xFF4C` (writeable only while the boot ROM is mapped; with the simulated boot it is set at reset: `0x04` = DMG-compat, `0x80` = CGB). `OPRI 0xFF6C` (bit 0; `1` in compat mode). `0xFF72`/`0xFF73` (R/W). `0xFF74` (R/W only in `cgb_mode`). `0xFF75` (bits 4-6 R/W, reads `0x8F|v`). `RP 0xFF56` (bits 0, 6-7 R/W, bit 1 reads 1 = "no light", reads `0x3C|v`). On DMG, or in compat mode, the CGB-only registers read `0xFF` and ignore writes (Review Focus 1).

- [ ] **Step 1: Write failing tests:** `"VBK switches the 0x8000 window between two banks"`, `"SVBK 0 selects bank 1"` (Review Focus 2), `"WRAM bank 0 at 0xC000 never switches"`, `"echo RAM 0xE000 mirrors the banked WRAM"`, `"CGB registers are invisible in DMG compat mode"` (writing `VBK` = 1 in compat mode → `peek(0xFF4F)==0xFF`, and `0x8000` still bank 0), `"RP reads no light"`.
- [ ] **Step 2: Run** them. Expected: FAIL.
- [ ] **Step 3: Implement** in `Bus::read_io`/`write_io` and the `0xC000-0xFDFF` routing.
- [ ] **Step 4: Run** all GB tests. Expected: PASS.
- [ ] **Step 5: Commit** `GB: CGB VRAM and WRAM banks, KEY0, OPRI, RP and the undocumented registers`

### Task 3: Colour palettes and an RGB555 framebuffer

**Files:** Modify `src/core/gb/ppu.hpp/.cpp`, `src/frontend/gameboy_system.cpp`, `tools/gb_runner/main.cpp`. Test: `tests/gb/test_cgb.cpp`, `tests/gb/test_ppu.cpp`.

**Interfaces:**
- Produces: `Ppu::framebuffer()` returns `std::span<const std::uint16_t>` (RGB555, bit 0-4 = red). In `Model::Dmg` the shades map to the fixed greys `{0x7FFF, 0x56B5, 0x294A, 0x0000}`. Existing shade-based PPU tests change to compare against these. `Ppu::bg_palette_ram()` / `obj_palette_ram()` return `std::span<const std::uint8_t, 64>` for the debugger and save states. `Ppu::set_compat_palettes(std::array<std::uint16_t,4> bg, obj0, obj1)` is used by Task 9.
- Registers: `BCPS 0xFF68` / `BCPD 0xFF69`, `OCPS 0xFF6A` / `OCPD 0xFF6B`. Bit 7 of the index register = auto-increment after a data *write* (not a read). The index register reads `0x40|v`. The data registers are inaccessible during mode 3 (reads `0xFF`, writes dropped, but the auto-increment still happens on a write). In compat mode, DMG `BGP`/`OBP0`/`OBP1` index into the compat palettes in palette RAM.

- [ ] **Step 1: Write failing tests:** `"BCPD auto-increments on write only"`, `"palette data is locked during mode 3"`, `"DMG model outputs fixed greys"`, `"compat mode maps BGP through palette RAM"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** The frontend converts RGB555 to its texture format; for now with no correction (Task 12 adds it). The runner's BMP writer expands 5-bit to 8-bit with `(c << 3) | (c >> 2)`.
- [ ] **Step 4: Run** the GB tests plus the dmg-acid2 screenshot comparison. Expected: PASS, and the image is identical to before.
- [ ] **Step 5: Commit** `GB: CGB colour palettes and an RGB555 framebuffer`

### Task 4: CGB tile attributes, sprite attributes and priority

**Files:** Modify `src/core/gb/ppu.cpp`. Test: `tests/gb/test_cgb.cpp`, plus the `cgb-acid2` ROM.

**Interfaces:** Consumes the VRAM banks (Task 2) and the palettes (Task 3).
- BG map attributes in VRAM bank 1 at the same map address: bits 0-2 palette, bit 3 tile bank, bit 5 X-flip, bit 6 Y-flip, bit 7 BG-to-OAM priority. These apply to the window too.
- OAM attribute bit 3 = tile bank, bits 0-2 = palette (only in `cgb_mode`).
- `cgb_mode` priority: if LCDC bit 0 = 0, sprites always win. Otherwise BG wins when (BG attr bit 7, or OAM bit 7) and the BG colour is not 0. Sprite-vs-sprite: OAM index order when `OPRI` bit 0 = 0 (CGB), X coordinate when it is 1 (DMG/compat). In `cgb_mode`, LCDC bit 0 does not blank the BG.

- [ ] **Step 1: Download `cgb-acid2`** (`cgb-acid2.gbc` and its reference `cgb-acid2.png`, from github.com/mattcurrie/cgb-acid2 releases) into `roms/`. Write the failing tests `"BG attribute flips and bank"` and `"LCDC bit 0 removes BG priority in CGB mode"`.
- [ ] **Step 2: Run** the tests, and run `ezy_gb_runner roms/cgb-acid2.gbc --screenshot out.bmp`. Expected: tests FAIL, and the image does not match the reference.
- [ ] **Step 3: Implement** inside `render_scanline()`. There is one renderer, with branches on `cgb_mode`.
- [ ] **Step 4: Run** the tests. Compare the screenshot with the reference pixel by pixel (convert the reference to BMP once; any small comparison script in `tools/` is fine). Expected: tests PASS, the image matches exactly, and dmg-acid2 is still identical.
- [ ] **Step 5: Commit** `GB: CGB tile and sprite attributes and priority rules (cgb-acid2)`

### Task 5: Access locking and variable mode-3 length (both models)

**Files:** Modify `src/core/gb/ppu.hpp/.cpp`, `src/core/gb/bus.cpp`. Test: `tests/gb/test_ppu.cpp`.

**Interfaces:**
- Produces: `Ppu::vram_accessible() const`, `Ppu::oam_accessible() const`. The bus checks them on CPU reads/writes: a blocked read returns `0xFF`, a blocked write is dropped. `peek`/`poke` and DMA ignore the locks. OAM is blocked in modes 2-3 and VRAM in mode 3, only while the LCD is on.
- Mode 3 length = `172 + (SCX % 8) + 6` if the window starts on the line, `+ 6..11` per sprite on the line (use the Pan Docs estimate). Mode 0 absorbs the rest of the 456 dots.

- [ ] **Step 1: Write failing tests:** `"VRAM reads 0xFF during mode 3"`, `"OAM reads 0xFF during mode 2"`, `"locks are lifted while the LCD is off"`, `"SCX fine scroll lengthens mode 3"`, `"sprites on the line lengthen mode 3"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Mode 3 length is computed at the start of mode 3.
- [ ] **Step 4: Run** all GB tests, `cpu_instrs`, dmg-acid2, cgb-acid2 and `rtc3test`. Expected: all PASS.
- [ ] **Step 5: Commit** `GB: VRAM/OAM access locking and variable mode 3 length`

### Task 6: VRAM DMA (GDMA and HDMA)

**Files:** Create `src/core/gb/hdma.hpp/.cpp` (add to `src/core/gb/CMakeLists.txt`). Modify `src/core/gb/bus.hpp/.cpp`, `src/core/gb/ppu.hpp` (an HBlank-entered signal). Test: `tests/gb/test_hdma.cpp`.

**Interfaces:**
- Produces: `class Hdma` holding `HDMA1-5` (`0xFF51-0xFF55`). Source mask `0xFFF0`, destination `0x8000 | (v & 0x1FF0)`, length `((v & 0x7F) + 1) * 16`. Writing bit 7 = 0 starts a GDMA (whole length, all at once). Bit 7 = 1 starts an HDMA (16 bytes at each HBlank start, LY 0-143, LCD on). Writing bit 7 = 0 during an active HDMA cancels it. `0xFF55` reads the remaining blocks − 1 (bit 7 = 0 while active), and `0xFF` when done. `Ppu::take_hblank_started()` reports each HBlank start.
- Timing: the CPU is stalled during the transfer, 2 bytes per M-cycle at normal speed (8 M-cycles per 16-byte block, 16 in double speed), advancing the rest of the system. An HDMA started while the LCD is off transfers one block right away and then nothing until HBlanks resume (Review Focus 3).

- [ ] **Step 1: Write failing tests:** `"GDMA copies length bytes and stalls the CPU"` (check `t_cycles` delta), `"HDMA copies 16 bytes per HBlank"`, `"0xFF55 reports remaining blocks"`, `"writing bit 7 = 0 cancels HDMA"`, `"HDMA with LCD off waits for the LCD"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** The bus runs pending transfers from `advance()`.
- [ ] **Step 4: Run** all GB tests. Expected: PASS.
- [ ] **Step 5: Commit** `GB: VRAM DMA (general and HBlank)`

### Task 7: Double speed (KEY1 + STOP)

**Files:** Modify `src/core/gb/cpu.cpp` (STOP), `src/core/gb/bus.hpp/.cpp`, `src/core/gb/timer.hpp`. Test: `tests/gb/test_cgb.cpp`.

**Interfaces:**
- Produces: `Bus::double_speed() const`. `KEY1 0xFF4D`: bit 0 armed (R/W), bit 7 current speed, reads `0x7E|...`. Only in `cgb_mode`.
- `STOP` (`0x10 0x00`): if armed, it toggles the speed, clears the armed bit, resets DIV and stalls for 2050 M-cycles. Otherwise it keeps today's behaviour, but resets DIV (Review Focus 5). `Bus::advance()` gives the PPU, APU and HDMA 2 dots per M-cycle in double speed and 4 at normal speed. The timer, serial and OAM DMA get one M-cycle each call. The APU frame sequencer stays on the DIV bit that keeps 512 Hz (bit 13 in double speed instead of bit 12).

- [ ] **Step 1: Write failing tests:** `"STOP with KEY1 armed switches to double speed"`, `"STOP without arming does not switch and resets DIV"`, `"a frame takes twice the CPU M-cycles in double speed"`, `"timer runs at CPU speed"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** `GameBoy::run_frame()` stays frame-based: it counts dots, not CPU cycles. Expose `Bus::dots()` for that.
- [ ] **Step 4: Run** all GB tests, plus Blargg `cpu_instrs.gb` and `instr_timing.gb` on CGB. Expected: PASS.
- [ ] **Step 5: Commit** `GB: CGB double speed mode`

### Task 8: CGB APU differences and PCM12/PCM34

**Files:** Modify `src/core/gb/apu.hpp/.cpp`. Test: `tests/gb/test_apu.cpp`.

**Interfaces:**
- `PCM12 0xFF76` / `PCM34 0xFF77` (read-only, CGB only): the current 4-bit digital output of channels 1|2 and 3|4. On CGB: length counters are **not** writable while the APU is off (on DMG they are), and wave RAM is freely accessible while channel 3 plays. The DMG wave-RAM corruption on retrigger does not happen. Use Pan Docs "Audio details" and the CGB notes from the Blargg `dmg_sound`/`cgb_sound` READMEs.

- [ ] **Step 1: Write failing tests:** `"PCM12 reports channel outputs"`, `"CGB length registers ignore writes while APU is off"`, `"DMG length registers accept writes while APU is off"` (a regression guard), `"CGB wave RAM is readable while playing"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** with a `Model` passed to the `Apu`.
- [ ] **Step 4: Run** all GB tests. Expected: PASS.
- [ ] **Step 5: Commit** `GB: CGB audio differences and PCM registers`

### Task 9: DMG compatibility palettes

**Files:** Create `src/core/gb/compat_palettes.hpp/.cpp`. Modify `src/core/gb/gameboy.cpp`. Test: `tests/gb/test_compat_palettes.cpp`.

**Interfaces:**
- Produces: `struct CompatPalette { std::array<std::uint16_t,4> bg, obj0, obj1; };`, `[[nodiscard]] CompatPalette select_compat_palette(const CartridgeHeader& header, std::optional<Button> held_direction, bool a_held, bool b_held);`
- Algorithm: if the licensee is Nintendo (old code `0x01`, or old `0x33` with new `"01"`), sum the 16 title bytes. Look the sum up in the boot ROM's checksum table, and break ties with the title's 4th letter. With no match, or a non-Nintendo licensee, use the default palette. The 12 button-combo palettes (direction ± A/B held at power-on) override the choice. Copy the tables (checksums, tie-break letters, palette triplets, combo palettes) from Pan Docs "Power Up Sequence → Compatibility palettes" as RGB555 constants, with a source comment.
- `GameBoy::create` calls it for DMG games on CGB and passes the result to `Ppu::set_compat_palettes`. The button combo comes from a new `Options::held_buttons` (`std::uint8_t` bitmask of `Button`). The frontend fills it from the keys held while loading.

- [ ] **Step 1: Write failing tests:** `"Nintendo title with known checksum gets its palette"` (use one entry from the table, e.g. the title `"POKEMON RED"`, and assert its triplet from Pan Docs), `"tie broken by 4th letter"`, `"non-Nintendo licensee gets the default palette"`, `"Left+A combo overrides"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all GB tests, plus the dmg-acid2 screenshot under `--model auto`. The compat palette is the default one, so compare after mapping the default palette back to shades. Expected: PASS.
- [ ] **Step 5: Commit** `GB: DMG compatibility palettes chosen like the CGB boot ROM`

### Task 10: Optional real CGB boot ROM

**Files:** Modify `src/core/gb/bus.hpp/.cpp`, `src/core/gb/gameboy.cpp`. Test: `tests/gb/test_cgb.cpp`.

**Interfaces:**
- `Options::boot_rom` (Task 1). It must be exactly 2304 bytes (`0x900`), otherwise `create` returns nullptr. While it is mapped (`0x0000-0x00FF` and `0x0200-0x08FF`, with the cartridge header visible at `0x0100-0x01FF`), the CPU starts at PC = 0 with zeroed registers. Writing any value to `0xFF50` unmaps it. `KEY0` is writable only while it is mapped. When the boot ROM unmaps itself, the compat palette selection of Task 9 is skipped (the real ROM did it).

- [ ] **Step 1: Write failing tests** with a synthetic 0x900-byte boot ROM (`LD A,0x11; LDH (0x50),A` at 0, rest NOP): `"boot ROM overlays 0x0000-0x00FF but not the header"`, `"writing 0xFF50 unmaps the boot ROM"`, `"wrong-size boot ROM is rejected"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** all GB tests. Expected: PASS.
- [ ] **Step 5: Commit** `GB: optional real CGB boot ROM`

### Task 11: GB save states (and a shared serializer)

**Files:** Move `src/core/gba/state.hpp` → `src/core/common/state.hpp` (namespace `ezy`; GBA includes are updated, and an INTERFACE target `ezy_state` is added in `src/core/CMakeLists.txt` or the nearest existing list). Modify every GB component, adding `template <class Ar> void serialize(Ar& ar)`: `cpu`, `bus`, `ppu`, `apu`, `timer`, `hdma`, `mbc` (including `Rtc`), `cartridge` (RAM vector), `joypad`. Modify `src/core/gb/gameboy.hpp/.cpp`. Test: `tests/gb/test_state.cpp`.

**Interfaces:**
- Produces: `std::vector<std::uint8_t> GameBoy::save_state();` and `bool GameBoy::load_state(std::span<const std::uint8_t>);`. Header: magic `"EZGB"`, `std::uint32_t version = 1`, `Model`, `cgb_mode`, and the header checksum plus global checksum of the ROM. Load into a temporary copy first, then commit only on success (Review Focus 4). Same pattern as `Gba::load_state`, read it first.

- [ ] **Step 1: Write failing tests:** `"save then load restores identical frames"` (run N frames, save, run M more, load, run M again; the framebuffers and `t_cycles` match), `"state from another ROM is rejected and machine is untouched"`, `"state from the other model is rejected"`, `"truncated state is rejected"`, `"wrong version is rejected"`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement**, then run the GBA tests too (the serializer moved).
- [ ] **Step 4: Run** `ctest --preset debug`. Expected: all PASS.
- [ ] **Step 5: Commit** `GB: save states; share the state serializer with GBA`

### Task 12: Frontend: model option, colour correction, F2/F4, CGB debugger

**Files:** Modify `src/frontend/gameboy_system.hpp/.cpp`, `src/frontend/main.cpp`, `tools/gb_runner/main.cpp`.

**Interfaces:**
- CLI (frontend and runner): `--model auto|dmg|cgb`, `--boot-rom <path>`. Frontend only: `--no-color-correction`.
- Colour correction (on by default, never for `Model::Dmg`): the SameBoy "modern balanced" curve, or the Near/byuu matrix: `r' = (r*26 + g*4 + b*2)`, `g' = (g*24 + b*8)`, `b' = (r*6 + g*4 + b*22)`, each `>> 2`, clamped to 255, plus the gamma step from the same source. Pick one, cite it in a comment, and put it in a pure function `std::uint32_t correct_cgb_color(std::uint16_t rgb555)` that is unit tested with white → white and black → black.
- F2 = save state, F4 = load state, one slot per ROM (`<rom>.state` next to the `.sav`), written atomically. Copy `GbaSystem::save_state`/`load_state`.
- Debugger panel: model, `cgb_mode`, speed, `VBK`/`SVBK`, `KEY1`, HDMA status, 8 BG + 8 OBJ palettes drawn as colour swatches, and the tile viewer with a bank-0/bank-1 toggle.

- [ ] **Step 1: Write the failing test** `"colour correction keeps white and black"` (put it next to the function; `tests/gb` is fine if the function lives in a small core-free header).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** everything above.
- [ ] **Step 4: Verify by hand:** start the frontend with `cgb-acid2.gbc`, check the colours look GBC-like, press F2 then F4, toggle the VRAM bank in the debugger. Start it with `dmg-acid2.gb --model dmg` and check it is grey. Expected: everything works.
- [ ] **Step 5: Commit** `GB: frontend model option, CGB colour correction, save state keys and CGB debugger`

### Task 13: Register the required test ROMs and run the final verification

**Files:** Modify `tests/gb/CMakeLists.txt`, and `roms/README.md` (where to download each test ROM).

- [ ] **Step 1: Download** into `roms/`: Blargg `instr_timing.gb`, the Mooneye `acceptance/boot_regs-cgb.gb` and the Mooneye `misc/` + `acceptance/` CGB-relevant ROMs.
- [ ] **Step 2: Add ctest entries** (Release, only `if(EXISTS ...)` like the current one): `cpu_instrs` with `--model cgb` and `--model dmg`, `instr_timing` with `--model cgb`, `boot_regs-cgb` with `--mooneye --model cgb`, and the cgb-acid2 / dmg-acid2 screenshot comparisons (runner flag `--compare <reference.bmp>`, exit 0 on an exact match; add it to the runner).
- [ ] **Step 3: Run** `cmake --build --preset release` and `ctest --preset release`. Expected: every required test passes.
- [ ] **Step 4: Run** the rest of the Mooneye CGB ROMs and write the pass/fail list into `roms/README.md` under "Known failures (dot-exact PPU, FIFO project)".
- [ ] **Step 5: Commit** `GB: register the CGB test ROMs`
