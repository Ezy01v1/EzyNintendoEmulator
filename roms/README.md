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

Still open, with what was learned:

- `ppu/hblank_ly_scx_timing-GS`: it only passes when leaving HALT for the interrupt takes
  one more M-cycle **and** the PPU starts at a different dot after boot (tried: env knobs).
  Either change alone breaks `halt_ime1_timing2-GS`, `di_timing-GS`,
  `halt_ime0_nointr_timing` and the `ppu/intr_2_*` ROMs, which were calibrated around the
  current HALT timing. Sampling IF part way through the M-cycle (an interrupt in the last
  dots seen one cycle later) did not help either.
- `ppu/lcdon_timing-GS`, `lcdon_write_timing-GS`: no value of the two LCD-on knobs in
  `ppu.cpp` (start dot 0..8, draw delay -4..4) passes. The expected tables say that on a
  normal line STAT shows mode 0 at least 250 dots after the line starts, while our mode 3
  ends at dot 249, and that LY changes at most 203 dots after mode 0 starts (ours: 204).
  Fixing that means recalibrating `kLineOriginOffsetDots`, the mode 3 dummy fetch and the
  HALT wake-up together against all the PPU ROMs.
