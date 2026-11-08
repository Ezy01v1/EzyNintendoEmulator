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

### Nintendo 64 RSP scalar core and SP DMA (2026-11-07)

With the same `n64-systemtest` ROM, **2824 of 3721** base tests pass. All scalar RSP, SP
register and `spmem: DMA` cases in this ROM pass. Vector instructions and RDP remain open.
The scalar execution and SP DMA use approximate timing. The 91 PeterLemon comparisons still
match their references.

### Nintendo 64 RSP vector unit (2026-11-07)

**2977 of 3721** base cases now pass in the same `n64-systemtest` ROM, 153 more than the
scalar build. All vector cases pass, including every 16-bit input for `VRCP` and `VRSQ`.
The two remaining cases labeled `RSP STATUS: start-valid` exercise the RDP freeze flag.
Vector instructions share the RSP's approximate instruction timing.

Instruction references include the SGI programmer's guide and hardware behavior documented
in ares. [SGI RSP Programmer's Guide](https://ultra64.ca/files/documentation/silicon-graphics/SGI_Nintendo_64_RSP_Programmers_Guide.pdf)
[ares vector interpreter](https://github.com/ares-emulator/ares/blob/master/ares/n64/rsp/interpreter-vpu.cpp)

### Nintendo 64 RDP command interface and fill rectangles (2026-11-07)

**2984 of 3721** base cases pass, seven more than the vector build. The RDP basic group
now passes, including both `start-valid` cases, incremental END writes and lists that wrap
inside DMEM. Integration tests also check the VI image, an XBUS list submitted by the RSP
and save-state resume inside a multiword command.

The command timing and whole-pixel fill clipping are approximate. Triangles, texture sampling,
depth and the general color pipeline remain open. [SGI RDP Command Summary](https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf)
[n64-systemtest RDP cases](https://github.com/lemmy-64/n64-systemtest/blob/main/src/tests/rdp/mod.rs)

### Nintendo 64 RDP fill triangles (2026-11-07)

The `0x08` triangle command draws solid spans in `FILL` cycle mode. Integration tests
check the pixel patterns for both major-edge directions, scissor clipping, signed
edge coefficients, XBUS wrap and save-state resume with part of the command buffered.
The installed `n64-systemtest` base ROM has no triangle cases. Single-cycle color,
shading, depth and textures remain open.

Edge setup and fill spans were checked against the command format and rasterizer
reference. [SGI RDP Command Summary](https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf)
[paraLLEl-RDP span setup](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/span_setup.comp)

### Nintendo 64 RDP opaque one-cycle triangles (2026-11-07)

The untextured `0x08` triangle command also draws opaque single-cycle spans into
RGBA5551 and RGBA8888. This path supports a constant RGB combiner input or direct
blend/fog color, eight-sample coverage with the non-AA sample rule, and ordered
RGB dithering. Integration tests compare pixels and coverage and restore the
color state with part of a triangle buffered.

Framebuffer reads, weighted blending, alpha comparison, noise dithering,
depth, shading and textures remain open. RGBA5551 writes the visible coverage bit.
The installed base ROM still serves as a regression check for the RDP interface.
[paraLLEl-RDP coverage](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/coverage.h)
[Angrylion framebuffer formats](https://github.com/ata4/angrylion-rdp-plus/blob/master/src/core/n64video/rdp/fbuffer.c)

### Nintendo 64 RDP texture copy (2026-11-07)

`Load Tile` and `Load Block` transfer RGBA5551 texels from RDRAM into TMEM.
The regular texture rectangle command in `COPY` mode reads those texels and
writes RGBA5551 pixels, with optional alpha testing. Integration tests compare
the resulting pixels after modifying source RDRAM, check odd-row layout and
TMEM wrapping, and restore TMEM with part of a rectangle command buffered.

All 131 N64 unit tests and all 91 PeterLemon cases registered in CTest pass.
The installed n64-systemtest ROM remains at 2984 of 3721 passing cases. Indexed
formats and palettes, filtering and textured triangles remain open. Command
timing and copy-mode scissor behavior are approximate.
[SGI RDP Command Summary](https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf)
[paraLLEl-RDP TMEM upload](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/tmem_update.comp)
[paraLLEl-RDP COPY sampler](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/texture.h)

### Nintendo 64 RDP indexed texture copy (2026-11-07)

`Load TLUT` expands 16-bit palette entries into the four banks of upper TMEM.
In `COPY` mode, CI4 selects a 16-entry palette through the tile's palette field,
and CI8 addresses all 256 entries. The 16-bit framebuffer receives the raw
palette entry. With alpha comparison enabled, its low bit controls whether
the pixel is written. Integration tests verify the packed uploads, palette
selection, per-bank lookup and save-state restoration with a partial command.
The save-state format is unchanged because it already stores all of TMEM.
All 133 N64 unit tests pass. The installed n64-systemtest ROM has the same
2984 of 3721 base cases passing as the RGBA5551 copy build.
[Nintendo programming manual](https://ultra64.ca/files/documentation/online-manuals/man/pro-man/pro13/13-09.html)
[Angrylion COPY pipeline](https://github.com/ata4/angrylion-rdp-plus/blob/master/src/core/n64video/rdp/rasterizer.c)

### Nintendo 64 RDP one-cycle point-sampled rectangles (2026-11-07)

Regular texture rectangles now support opaque one-cycle rendering from RGBA5551
or indexed CI4/CI8 textures with RGBA16 or IA16 palettes. The RGB combiner can
pass TEXEL0 through or multiply it by the primitive color. This path supports
RGBA5551 and RGBA8888 framebuffers, tile coordinate shift, clamp and mirroring,
and the existing coverage and ordered dithering rules. Tests check scaling,
scissor clipping, fractional coverage, indexed color, and save-state
restoration with part of a rectangle command buffered.

All 135 N64 unit tests pass. The installed n64-systemtest ROM retains the same
2984 of 3721 passing base cases as the indexed copy build.
Texture interpolation and pipeline timing are approximate.
[SGI RDP Command Summary](https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf)
[paraLLEl-RDP texture sampler](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/texture.h)
[paraLLEl-RDP span setup](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/span_setup.comp)

### Nintendo 64 RDP one-cycle filtered rectangles (2026-11-07)

Opaque texture rectangles support the three-tap filter for RGBA5551 and
indexed CI4/CI8 textures with RGBA16 or IA16 palettes. The five-bit S and T
fractions choose the interpolation triangle and its weights. With
`G_TF_AVERAGE`, the exact midpoint averages four texels. Each tap uses the
tile's coordinate addressing and the palette bank selected by the filter.

Integration tests check both interpolation triangles, the diagonal and
midpoint, tile-edge clamping, wrapping, mirroring and palette banks. All
137 N64 unit tests pass. The installed n64-systemtest ROM retains 2984 of
3721 passing base cases.
[Nintendo texture filter reference](https://ultra64.ca/files/documentation/online-manuals/man/n64man/gdp/gDPSetTextureFilter.html)
[Angrylion texture filtering](https://github.com/ata4/angrylion-rdp-plus/blob/master/src/core/n64video/rdp/tex.c)
[paraLLEl-RDP texture sampler](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/texture.h)

### Nintendo 64 RDP affine textured triangles (2026-11-07)

The unshaded `0x0A` command draws opaque one-cycle triangles with affine S
and T interpolation. The triangle shares the rectangle's point and
three-tap samplers, its TEXEL0 and primitive-color modulation paths, and
the RGBA5551/RGBA8888 framebuffer writer. The texture origin follows the
unclipped major edge. Tests check fractional coefficients, both major-edge
orientations, clipping, coverage and indexed filtering.

Save-state version 8 stores all twelve buffered words. Earlier states
retain their original four-word layout and skip counts. Tests resume a
partially buffered triangle and a version 7 RDP state with an in-flight skip.
All 140 N64 unit tests pass. The installed n64-systemtest ROM retains 2984 of
3721 passing base cases.
[SGI RDP Command Summary](https://ultra64.ca/files/documentation/silicon-graphics/SGI_RDP_Command_Summary.pdf)
[Angrylion triangle rasterizer](https://github.com/ata4/angrylion-rdp-plus/blob/master/src/core/n64video/rdp/rasterizer.c)

### Nintendo 64 RDP perspective-correct textured triangles (2026-11-07)

Opaque one-cycle `0x0A` triangles interpolate W with the same fixed-point
scanline setup as S and T. With perspective enabled, the RDP's quantized
reciprocal projects S and T before tile shifts, clamping and sampling.
Integration tests cover point and filtered sampling, horizontal and vertical
W gradients, fractional coefficients, reciprocal rounding, and saturation
with very small or nonpositive W. All 142 N64 unit tests pass. The installed
n64-systemtest ROM retains 2984 of 3721 passing base cases.

Perspective correction uses the selected tile. Texture LOD, shading, depth,
and one-cycle alpha comparison and blending remain open.
[paraLLEl-RDP perspective divider](https://github.com/Themaister/parallel-rdp/blob/master/parallel-rdp/shaders/perspective.h)
[Angrylion texture coordinates](https://github.com/ata4/angrylion-rdp-plus/blob/master/src/core/n64video/rdp/tcoord.c)
