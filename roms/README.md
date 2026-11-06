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
