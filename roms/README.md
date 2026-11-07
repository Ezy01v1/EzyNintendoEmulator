# roms/

Put your own ROMs / test ROMs here. Everything in this folder except this file is git-ignored.

## Game Boy / Game Boy Color test ROMs

`ctest --test-dir build/default -C Release` runs each of these when it is present:

| File | Source |
|------|--------|
| `cpu_instrs.gb`, `instr_timing.gb` | Blargg's tests, e.g. github.com/retrio/gb-test-roms |
| `dmg-acid2.gb` + `dmg-acid2-reference-dmg.bmp`, `dmg-acid2-reference-cgb.bmp` | github.com/mattcurrie/dmg-acid2 (convert `img/reference-dmg.png` / `reference-cgb.png` to 24-bit BMP) |
| `cgb-acid2.gbc` + `cgb-acid2-reference.bmp` | github.com/mattcurrie/cgb-acid2 (release v1.1; convert `img/reference.png`) |
| `mooneye/` | Mooneye Test Suite build `mts-20240926-1737-443f6e1` from gekkio.fi, extracted here |

### Known Mooneye failures (2026-11-06)

`--model cgb`: 54 of 111 ROMs pass, `--model dmg`: 54 (one more than before the CGB work).
The remaining failures are not CGB regressions. The DMG model fails the same ones.

- Exact CPU memory-access timing inside instructions (`call_*`, `jp_*`, `ret*_timing`,
  `push_timing`, `rst_timing`, `add_sp_e_timing`, `ld_hl_sp_e_timing`, `ie_push`).
- OAM DMA is instant (`oam_dma_start`, `oam_dma_restart`, `oam_dma_timing`).
- Dot-exact PPU timing, left for the pixel-FIFO project (`ppu/intr_2_*`, `ppu/stat_lyc_onoff`,
  `misc/ppu/vblank_stat_intr-C`).
- TIMA reload delay (`timer/tima_reload`, `tima_write_reloading`, `tma_write_reloading`).
- MBC2 and MBC1 multicarts are not supported (`emulator-only/mbc2/*`, `mbc1/multicart_rom_8Mb`).
- Exact CGB power-on DIV and I/O values (`misc/boot_div-cgb*`, `misc/boot_hwio-C`,
  `misc/bits/unused_hwio-C`).

### Mooneye baseline before the accuracy pass (2026-11-06)

Passing ROMs (`--mooneye 10`):

- **dmg** (54): acceptance/bits/mem_oam, acceptance/bits/reg_f, acceptance/boot_regs-dmgABC, acceptance/di_timing-GS, acceptance/div_timing, acceptance/ei_sequence, acceptance/ei_timing, acceptance/halt_ime0_ei, acceptance/halt_ime0_nointr_timing, acceptance/halt_ime1_timing, acceptance/halt_ime1_timing2-GS, acceptance/if_ie_registers, acceptance/instr/daa, acceptance/intr_timing, acceptance/oam_dma/basic, acceptance/oam_dma/reg_read, acceptance/pop_timing, acceptance/ppu/hblank_ly_scx_timing-GS, acceptance/ppu/intr_1_2_timing-GS, acceptance/ppu/intr_2_0_timing, acceptance/ppu/stat_irq_blocking, acceptance/rapid_di_ei, acceptance/reti_intr_timing, acceptance/timer/div_write, acceptance/timer/rapid_toggle, acceptance/timer/tim00, acceptance/timer/tim00_div_trigger, acceptance/timer/tim01, acceptance/timer/tim01_div_trigger, acceptance/timer/tim10, acceptance/timer/tim10_div_trigger, acceptance/timer/tim11, acceptance/timer/tim11_div_trigger, emulator-only/mbc1/bits_bank1, emulator-only/mbc1/bits_bank2, emulator-only/mbc1/bits_mode, emulator-only/mbc1/bits_ramg, emulator-only/mbc1/ram_256kb, emulator-only/mbc1/ram_64kb, emulator-only/mbc1/rom_16Mb, emulator-only/mbc1/rom_1Mb, emulator-only/mbc1/rom_2Mb, emulator-only/mbc1/rom_4Mb, emulator-only/mbc1/rom_512kb, emulator-only/mbc1/rom_8Mb, emulator-only/mbc2/bits_unused, emulator-only/mbc5/rom_16Mb, emulator-only/mbc5/rom_1Mb, emulator-only/mbc5/rom_2Mb, emulator-only/mbc5/rom_32Mb, emulator-only/mbc5/rom_4Mb, emulator-only/mbc5/rom_512kb, emulator-only/mbc5/rom_64Mb, emulator-only/mbc5/rom_8Mb
- **cgb** (54): acceptance/bits/mem_oam, acceptance/bits/reg_f, acceptance/di_timing-GS, acceptance/div_timing, acceptance/ei_sequence, acceptance/ei_timing, acceptance/halt_ime0_ei, acceptance/halt_ime0_nointr_timing, acceptance/halt_ime1_timing, acceptance/halt_ime1_timing2-GS, acceptance/if_ie_registers, acceptance/instr/daa, acceptance/intr_timing, acceptance/oam_dma/basic, acceptance/oam_dma/reg_read, acceptance/pop_timing, acceptance/ppu/hblank_ly_scx_timing-GS, acceptance/ppu/intr_1_2_timing-GS, acceptance/ppu/intr_2_0_timing, acceptance/ppu/stat_irq_blocking, acceptance/rapid_di_ei, acceptance/reti_intr_timing, acceptance/timer/div_write, acceptance/timer/rapid_toggle, acceptance/timer/tim00, acceptance/timer/tim00_div_trigger, acceptance/timer/tim01, acceptance/timer/tim01_div_trigger, acceptance/timer/tim10, acceptance/timer/tim10_div_trigger, acceptance/timer/tim11, acceptance/timer/tim11_div_trigger, emulator-only/mbc1/bits_bank1, emulator-only/mbc1/bits_bank2, emulator-only/mbc1/bits_mode, emulator-only/mbc1/bits_ramg, emulator-only/mbc1/ram_256kb, emulator-only/mbc1/ram_64kb, emulator-only/mbc1/rom_16Mb, emulator-only/mbc1/rom_1Mb, emulator-only/mbc1/rom_2Mb, emulator-only/mbc1/rom_4Mb, emulator-only/mbc1/rom_512kb, emulator-only/mbc1/rom_8Mb, emulator-only/mbc2/bits_unused, emulator-only/mbc5/rom_16Mb, emulator-only/mbc5/rom_1Mb, emulator-only/mbc5/rom_2Mb, emulator-only/mbc5/rom_32Mb, emulator-only/mbc5/rom_4Mb, emulator-only/mbc5/rom_512kb, emulator-only/mbc5/rom_64Mb, emulator-only/mbc5/rom_8Mb, misc/boot_regs-cgb

Performance (`python -I tools/gb_bench.py`, Release, median of 3, 3600 frames):

- cgb-acid2.gbc: 9.78 s for 60.3 s emulated = 16.2 % of one core
- pokemon-silver.gbc: 15.11 s for 60.3 s emulated = 25.1 % of one core

### After the accuracy pass (2026-11-06)

Mooneye: **80** pass with `--model dmg`, **80** with `--model cgb` (54 and 54 before). No ROM that passed before fails now. 46 of the timing ROMs are in ctest.

Still failing among the ones the design required:

- `ppu/hblank_ly_scx_timing-GS`: **regression from this pass.** It passed before, and started failing when interrupt sampling moved to the end of the opcode fetch (needed for the timer ROMs). Open item.
- `ppu/lcdon_timing-GS`, `ppu/lcdon_write_timing-GS`: line 0 right after the LCD is switched on needs its own offsets (knobs in `ppu.cpp`, currently 0).
- `misc/ppu/vblank_stat_intr-C` (CGB).

Other failures are boot-state ROMs for other models (`boot_*`), MBC2 and the MBC1 multicart.

Performance (same method as the baseline):

- cgb-acid2.gbc: 19.97 s for 60.3 s emulated = 33.1 % of one core
- pokemon-silver.gbc: 19.13 s for 60.3 s emulated = 31.7 % of one core

Real speed needs 100 %; both stay far below. The 25 % soft goal is missed: the per-dot PPU loop costs about 7 points on Pokémon Silver. The obvious optimisation is to skip the per-dot loop in modes 0, 1 and 2, where nothing happens dot by dot.

### Performance pass and CGB VBlank STAT (2026-11-06, another PC)

Same method; the baseline was re-measured on this PC before the change.

- Outside mode 3 the PPU skips straight to the next dot where something happens (dots 1,
  80, 452, 453, 456). Mooneye results and Pokémon Silver screenshots are identical.
  - pokemon-silver.gbc: 24.1 % → **12.0 %** of one core
  - cgb-acid2.gbc: 11.1 % → 10.6 %
- `misc/ppu/vblank_stat_intr-C` passes: on a CGB the line 144 mode 2 STAT source rises one
  M-cycle before VBlank. `acceptance/ppu/vblank_stat_intr-GS` now fails with `--model cgb`,
  which is what CGB hardware does too; it still passes with `--model dmg`.
- Mooneye: 81 of 115 ROMs pass on both models (115 counts every `.gb` in the suite).

What was still open then is fixed by the next pass.

### PPU line timing recalibration (2026-11-06)

The model was derived from the expected-value tables of `lcdon_timing-GS` and
`lcdon_write_timing-GS` (they read LY, STAT, OAM and VRAM at known cycles after LCD on),
then checked against every timing ROM. In our timing frame (CPU accesses see the PPU after
the 4 dots of their M-cycle):

- LY shows the next line 4 dots before the line ends (was 3). STAT bit 2 reads 0 until the
  LY=LYC comparator catches up one dot later. LY reads 153 for exactly one M-cycle.
- OAM reads are locked 4 dots before a visible line's OAM scan; VRAM reads 4 dots before
  mode 3. OAM writes pass in the 4 dots between the end of the OAM scan and mode 3. CPU
  reads and writes have separate locks.
- Mode 3 is 172 dots without sprites. Mode 0 starts 3 dots after the last pixel, and
  sprite fetches absorb those dots, which keeps `intr_2_mode0_timing_sprites` passing.
- HALT is unchanged: `halt_ime1_timing2-GS` proves it has no extra delay.

Results:

- `hblank_ly_scx_timing-GS`, `lcdon_timing-GS`, `lcdon_write_timing-GS` pass and run in
  ctest. Mooneye: **84** of 115 on both models (81 before), no ROM that passed fails now.
- Pokémon Silver screenshots (frames 900, 1500, 2400, 3600) are identical to before.
- Performance, both builds measured alternately on the same PC: Pokémon Silver 13.5-14.4 %
  before, 14.8-14.9 % after (mode 3 is 3 dots longer and LYC is compared every dot).

Remaining failures are all outside the DMG/CGB timing work: boot-state ROMs for other
models (`boot_div-*`, `boot_hwio-*`, `boot_regs-*` for S/MGB/dmg0/SGB), `unused_hwio`,
MBC2 and the MBC1 multicart.

## Nintendo 64 test ROMs

| Path | Source |
|------|--------|
| `n64/peterlemon/<Group>/<Test>/*.N64` + `.bmp` | github.com/PeterLemon/N64, folder `CPUTest` (prebuilt `.N64` files). Each `.png` reference is converted with `ffmpeg -i x.png -pix_fmt bgr24 x.bmp` (640×480). `CP0/COP0Register` and the two `TIMINGNTSC` tests have no reference image. `CPUTest/Exceptions` ships only sources (no prebuilt ROMs). |
| `n64/n64-systemtest.z64` | Built from github.com/lemmy-64/n64-systemtest (see the N64 results section for the commit). |

### Nintendo 64 results (subproject 1, 2026-11-07)

**PeterLemon CPUTest:** 91 of 94 ROMs with a reference image match it pixel for pixel and
run in ctest (`n64_peterlemon_*`). Left out, documented in `tests/n64/CMakeLists.txt`:
`CPU/LOADSTORE/SB` and `SH` (their references draw the last dashed line 1 pixel lower than
the same font does in every other test: stale images) and `DMAAlignment-PI-ROM-FROM` (one
cell reads PI_DRAM_ADDR as 0x80024408 right after a DMA, while the same test then checks
0x24408). `CP0/COP0Register` and the two `TIMINGNTSC` tests have no reference image.

**n64-systemtest** (commit `196f542`, `ezy_n64_runner --isviewer`): runs to the end,
**Base: 2659 of 3721 pass**. Not in ctest yet: the gate would fail until the RSP exists.
The failures:

- RSP and SP (subproject 2): every `RSP …`, `SP …` and `spmem: DMA …` test.
- RDP (subproject 3): `RDP …`.
- Caches (out of scope by design, the cache contents aren't emulated): `CACHE …`,
  `ICACHE …`, `DCACHE …`.
- Still open in the CPU/bus core:
  - FPU: `64 bit with odd indices (half mode)` (+ conversions), `Comparisons in half mode
    with odd register indices`, `Upper bits of 32 bit operation/conversions (half mode)`
    (the FR=0 register addressing of arithmetic is still not exact), and the randomized
    `ADD/SUB/MUL/DIV/SQRT.S`, `MUL/DIV.D` (rounding or flush details).
  - `TLB: Store misaligned after 4k page, expect TLBS`.
  - `COP2Usable (DCFC2/DCTC2)`.
  - Sub-word writes to RCP memory: `spmem: SB/SH/SD`, `pifram: SB/SH`.
  - `MI Repeat: …` (MI_MODE repeat mode), `RE …` (reverse-endian user mode),
    `cart-writing: …` (the PI write latch on the cartridge bus).

**Performance** (Release, this PC, median of 3): `CPUADD.N64` 600 frames (10 s emulated)
in 4.86 s. n64-systemtest's base set takes about 13 minutes of real time (much of it waiting
on an RSP that never runs).
