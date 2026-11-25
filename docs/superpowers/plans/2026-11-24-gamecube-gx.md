# GameCube Sub-project 2 (GX on Direct3D 11) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw what the GameCube GX receives on a Direct3D 11 device, copy the EFB back to RAM like the console, and bring Mario Kart: Double Dash!! to a visible title screen.

**Architecture:** `ezy_gc` gains a GX state, vertex loader, CPU-side transform/lighting/texgen, texture decoder and TEV key, all API-free, behind a `gx::Backend` interface. A separate library `ezy_gc_d3d11` implements it with a headless D3D11 device (hardware, or WARP for tests). The EFB is read back on every copy, so the frontend and the VI keep showing the XFB from RAM, unchanged.

**Tech Stack:** C++20, MSVC Build Tools 2022, CMake presets, doctest, Direct3D 11 + DXGI + D3DCompiler from the Windows SDK 10.0.26100.

**Spec:** `docs/superpowers/specs/2026-11-24-gamecube-gx-design.md`

## Global Constraints

- Branch `gc/gx` (from `main`). Code, comments and commits in English; docs and notes in Spanish.
- Build only with presets: `cmake --preset default` (when CMake files change), `cmake --build --preset debug|release`, `ctest --preset debug`. CMake path: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`. `/W4` clean.
- No new dependencies. D3D11, DXGI and D3DCompiler come from the Windows SDK and Windows (`d3dcompiler_47.dll`, `d3d10warp.dll`). `ezy_gc` itself must not include or link Direct3D.
- Nothing is silently ignored. An unimplemented BP/XF/CP register, texture format, TEV setting or EFB access stops the machine with an error naming it (the existing `BusError`/`GekkoError` pattern, with the PC). Store-only registers carry an "accepted, no effect" comment.
- No per-game hacks. Sources for GX behavior: Dolphin's VideoCommon (master, on GitHub), cited in comments.
- Rendering tests run on WARP and compare exact pixel values. Hardware is the default device in ezyemu and the runner (`--gx-warp` selects WARP).
- Other consoles and the frontend's renderer do not change. The existing suites keep passing, and the gc Debug suite stays well under its 120 s timeout.
- MKDD: `E:/Rom nintendo/Mario Kart Double Dash/Mario Kart - Double Dash!! (USA).nkit.iso`. Never commit it.
- Python/bash heredocs turn `\n` inside C++ string literals into real newlines, so use the Edit tool for C++ strings.
- Do not leave test runs in the background. Two concurrent `ctest` runs starve each other on this machine.

## Review Focus

1. Primitive types other than triangles must become the right triangles, with the right winding for culling: quads, strips, fans, and lines/points (which stop with an error until implemented). Task 3 test "every primitive type yields the expected triangles".
2. Indexed vertex attributes read from RAM must use the array base and stride registers, with 8- and 16-bit indices. An index past MEM1 stops with an error. Task 1 test "indexed attributes read base + index × stride".
3. A texture whose RAM contents change between draws must not be drawn from a stale cache entry. Task 4 test "a texture rewritten in RAM is decoded again".
4. EFB copies must honour the source rectangle, the destination stride and the vertical scale, and an interlaced field copy must write only its rows. Task 2 test "XFB copy writes the rectangle at the stride", Task 6 test "texture copy encodes the format at the destination".
5. A device lost or a shader that fails to compile must stop with a clear error, not draw garbage. Task 2 test "a backend failure surfaces as an error", Task 5 test "every generated shader compiles".

---

### Task 1: GX state and vertex loader

**Files:**
- Create: `src/core/gc/gx/gx_state.hpp/.cpp` (CP, XF and BP register files with decoded accessors), `src/core/gc/gx/vertex_loader.hpp/.cpp`
- Modify: `src/core/gc/gx_fifo.hpp/.cpp` (hand every LOAD_CP/XF/BP/INDX command and every primitive to the state and loader instead of only sizing them), `src/core/gc/CMakeLists.txt`
- Test: `tests/gc/test_gx_state.cpp`, `tests/gc/test_vertex_loader.cpp`

**Interfaces:**
- Produces:
  - `struct gx::LoadedVertex { float pos[3]; float normal[3], binormal[3], tangent[3]; std::uint32_t color[2] /* RGBA8 */; float tex[8][2]; std::uint8_t pos_matrix; std::uint8_t tex_matrix[8]; bool has_normal; std::uint8_t color_count, tex_count; };`
  - `class gx::State` with `void load_cp(std::uint8_t reg, std::uint32_t value)`, `void load_xf(std::uint16_t addr, std::span<const std::uint32_t> values)`, `void load_indexed(int which /*A..D*/, std::uint32_t word)`, `void load_bp(std::uint32_t word)`. Each throws `GxError` (carrying the register or address, then wrapped with the PC like BusError) for registers not modelled. It also has read accessors for the decoded fields later tasks need: VCD/VAT, array bases/strides, XF matrices, viewport, projection, channels, texgen, and the BP state.
  - `std::vector<gx::LoadedVertex> gx::load_vertices(const State&, std::uint8_t vat, std::span<const std::uint8_t> data, std::uint16_t count, std::span<const std::uint8_t> mem1)`
  - `GxFifo` gains a sink: `void set_draw_sink(std::function<void(std::uint8_t primitive, std::uint8_t vat, std::vector<gx::LoadedVertex>&&)>)` and `void set_bp_sink(std::function<void(std::uint32_t)>)` for BP commands with side effects (EFB copy, clears, draw done), so Task 2 can wire the backend.
- Formats: positions XY/XYZ × u8/s8/u16/s16/f32 with the VAT fraction shift; normals N/NBT/NBT3 × s8/s16/f32 (scales 6/14); colors RGB565/RGB8/RGBX8/RGBA4/RGBA6/RGBA8; texcoords S/ST × the 5 types with their shift; position/texture matrix indices; direct, 8-bit index and 16-bit index. Source: Dolphin VertexLoader*/CPMemory.

- [ ] **Step 1: Write the failing tests:** `TEST_CASE("each position, normal, color and texcoord format decodes with its fraction shift")`; `TEST_CASE("indexed attributes read base + index × stride")` with 8- and 16-bit indices, and an index past MEM1 that throws; `TEST_CASE("XF loads fill matrices, viewport and projection")`; `TEST_CASE("an unmodelled BP register stops with its address")`; `TEST_CASE("the FIFO hands primitives to the draw sink")`.
- [ ] **Step 2: Run them to see them fail** (compile errors). Command: `cmake --build --preset debug --target ezy_gc_tests`
- [ ] **Step 3: Implement.** Keep `GxFifo`'s existing sizing and counters. The loader's per-vertex size must equal the size `gx_vertex` already computes (assert it).
- [ ] **Step 4: Run** `ctest --preset debug -R ezy_gc_tests`. Expect PASS. Then check that the MKDD boot test still reaches its main loop: with no sink set, the FIFO behaves as today.
- [ ] **Step 5: Commit.** Message: `GameCube GX: register state and vertex loader`

### Task 2: Backend interface, D3D11 device and the EFB copy to the XFB

**Files:**
- Create: `src/core/gc/gx/backend.hpp` (interface + `ClipVertex` + `DrawState`), `src/core/gc/gx/null_backend.cpp`, `src/core/gc/gx/efb_copy.hpp/.cpp` (RGBA→YUV XFB writer, moved from gx_fifo's clear path and generalised)
- Create: `src/core/gc/gx_d3d11/CMakeLists.txt` (library `ezy_gc_d3d11`, links `d3d11 dxgi d3dcompiler`, depends on `ezy_gc`), `src/core/gc/gx_d3d11/d3d11_backend.hpp/.cpp`
- Modify: `src/core/gc/gamecube.hpp/.cpp` (`create(path, std::unique_ptr<gx::Backend>)`; wire the draw/BP sinks), `tools/gc_runner` (D3D11 by default, `--gx-warp`), `src/frontend/gamecube_system.cpp` (D3D11 hardware), the top `CMakeLists.txt`
- Test: `tests/gc_d3d11/CMakeLists.txt` (`ezy_gc_d3d11_tests`, ctest entry), `tests/gc_d3d11/test_d3d11_backend.cpp`, `tests/gc/test_efb_copy.cpp`

**Interfaces:**
- Consumes: Task 1's `gx::State`, `LoadedVertex`, sinks.
- Produces:
  - `struct gx::ClipVertex { float pos[4]; float color[2][4]; float tex[8][3]; };`
  - `struct gx::DrawState` (filled in by Tasks 3–5): the viewport, scissor, depth (enable, func, update), blend (mode, factors, logic op, color/alpha update), texture slots (`TextureHandle` ×8), `PixelShaderKey`, and the EFB pixel format.
  - `class gx::Backend { virtual ~Backend(); virtual void draw(const DrawState&, std::span<const ClipVertex> triangles) = 0; virtual void clear(Rect, std::uint32_t rgba, std::uint32_t z, bool color, bool alpha, bool depth) = 0; virtual void read_color(Rect, std::span<std::uint32_t> rgba) = 0; virtual void read_depth(Rect, std::span<std::uint32_t> z24) = 0; virtual TextureHandle texture(const TextureKey&, int w, int h, std::span<const std::uint32_t> rgba8) = 0; };` A backend error throws `gx::BackendError(std::string)`.
  - `std::unique_ptr<gx::Backend> gx::make_null_backend()`: it counts draws and reads back black. It is used only by tests that don't render; its header comment says so.
  - `std::unique_ptr<gx::Backend> gx::make_d3d11_backend(bool warp)`: a headless device, the EFB as a 640×528 RGBA8 target plus a D24S8 depth buffer, and readback through a staging texture.
  - `void gx::write_xfb_copy(std::span<std::uint8_t> mem1, std::uint32_t dest, std::uint32_t stride_bytes, Rect src, float y_scale, std::span<const std::uint32_t> rgba)`: BT.601 YUV 4:2:2, Dolphin's `TextureConversionShader` XFB encode, with gamma.
- In this task the draw path has no transform yet: Task 3 adds it. For now the runner's first MKDD frame comes from clears and copies alone; the triangle test feeds `ClipVertex`es directly.

- [ ] **Step 1: Write the failing tests.**
  - `TEST_CASE("WARP draws a vertex-coloured triangle")`: three `ClipVertex`es covering the left half, with red/green/blue corners. Read back and check the corner pixels and that one pixel outside the triangle is untouched.
  - `TEST_CASE("clear sets color and depth inside the rectangle only")`.
  - `TEST_CASE("XFB copy writes the rectangle at the stride")`: a 4×2 source, stride 1280, y_scale 1. Check exact YUV bytes and that bytes outside the rectangle are untouched.
  - `TEST_CASE("a backend failure surfaces as an error")`: make the device fail on purpose, for example with an invalid texture size, and expect `BackendError`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.** Wire BP `0x52` (EFB copy to XFB, with or without clear) to `read_color` + `write_xfb_copy`. The old "clear color only" path now goes through the backend.
- [ ] **Step 4: Run** `ctest --preset debug -R "ezy_gc_tests|ezy_gc_d3d11_tests"`. Expect PASS. Then run the MKDD runner with `--fields 200 --screenshot build/gc/gx_t2.bmp --gx-warp` and record in the progress doc what the screenshot shows.
- [ ] **Step 5: Commit.** Message: `GameCube GX: backend interface, D3D11 device and EFB copy to the XFB`

### Task 3: Transform, lighting, texgen, primitives, culling, scissor, depth

**Files:**
- Create: `src/core/gc/gx/transform.hpp/.cpp` (XF on the CPU: position/normal matrices, projection orthographic/perspective, viewport to clip space, the color channels with lighting (ambient/material sources, diffuse/attenuation functions, spot/distance attenuation), texgen (regular, emboss/bump if seen, color-to-texcoord, normalisation, post-transform matrices)), `src/core/gc/gx/primitives.hpp/.cpp`
- Modify: `src/core/gc/gamecube.cpp` (draw sink → transform → backend), `d3d11_backend.cpp` (depth state, scissor, viewport)
- Test: `tests/gc/test_transform.cpp`, `tests/gc/test_primitives.cpp`, `tests/gc_d3d11/test_d3d11_draw.cpp`

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces:
  - `std::vector<gx::ClipVertex> gx::transform(const gx::State&, std::span<const gx::LoadedVertex>)`
  - `std::vector<gx::ClipVertex> gx::assemble(std::uint8_t primitive, std::span<const gx::ClipVertex>, gx::CullMode)`: triangle list output. Quads become 2 triangles each, strips and fans unroll with the console's winding, culling is applied on the CPU, and lines/points throw until implemented.
  - DrawState fields: viewport, scissor (BP `0x20/0x21` plus the offset `0x59`), depth (BP `0x40` ZMODE), and depth range from the viewport.
- The post-transform matrices are identity at boot. Dolphin's BS2 sets them; this was deferred from sub-project 1. Add that to `hle_boot`.

- [ ] **Step 1: Write the failing tests:** `TEST_CASE("every primitive type yields the expected triangles")`; `TEST_CASE("perspective and orthographic projections match Dolphin's math")` with hand-computed clip coordinates; `TEST_CASE("lighting: ambient, diffuse clamp, attenuation")`; `TEST_CASE("texgen from position, normal and color with post matrix")`; `TEST_CASE("WARP: depth test keeps the nearer triangle")`; `TEST_CASE("WARP: scissor clips")`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the gc and gc_d3d11 tests and expect PASS. Then run MKDD with `--gx-warp --fields 400`, save a screenshot, and record what it shows in the progress doc (untextured geometry is expected).
- [ ] **Step 5: Commit.** Message: `GameCube GX: transform, lighting, texgen, primitives, culling, scissor, depth`

### Task 4: Textures

**Files:**
- Create: `src/core/gc/gx/texture_decode.hpp/.cpp` (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, C4/C8/C14X2 with a TLUT in IA8/RGB565/RGB5A3; tiled layouts), `src/core/gc/gx/texture_cache.hpp/.cpp`
- Modify: `gx_state` (BP texture registers 0x80–0xBB: image 0–3, LOD, mode, TLUT loads 0x64/0x65 into TMEM, and the preloads into TMEM if seen), `d3d11_backend.cpp` (samplers: wrap/clamp/mirror, nearest/linear, LOD bias and min/max LOD)
- Test: `tests/gc/test_texture_decode.cpp`, `tests/gc_d3d11/test_d3d11_texture.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - `std::vector<std::uint32_t> gx::decode_texture(TextureFormat, int w, int h, std::span<const std::uint8_t> data, std::span<const std::uint16_t> tlut, TlutFormat)`, returning RGBA8 rows.
  - `gx::TextureCache::get(const gx::State&, int slot, std::span<const std::uint8_t> mem1, gx::Backend&) -> TextureHandle`. It is keyed by address, format, size, TLUT and a hash of the bytes, so a texture rewritten in RAM is decoded again.
- Mipmaps: decode level 0. Upload further levels only if MKDD's textures use them (an unsupported LOD setting stops with an error).

- [ ] **Step 1: Write the failing tests:** one exact `TEST_CASE` per format with a tiny hand-made tile (`"I4 decodes 8x8 tiles"`, ..., `"CMPR decodes DXT1-like blocks with GameCube's color order"`, `"C8 with an RGB5A3 TLUT"`); `TEST_CASE("a texture rewritten in RAM is decoded again")`; `TEST_CASE("WARP: textured quad with clamp and repeat")`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the tests and expect PASS. Run the MKDD screenshot and update the progress doc.
- [ ] **Step 5: Commit.** Message: `GameCube GX: texture formats, TLUT, cache and sampling`

### Task 5: TEV, alpha test, blending, fog, EFB pixel formats

**Files:**
- Create: `src/core/gc/gx/pixel_key.hpp/.cpp` (the key from BP state: up to 16 TEV stages with color/alpha inputs, operations (add/sub, compare modes), bias, scale, clamp, destination registers, swap tables, konst selections, ras/tex selection per stage, indirect texturing (if MKDD uses it; otherwise stop with an error), the alpha compare with logic, fog (type and range adjustment if used), the EFB format quantisation and dithering if enabled)
- Create: `src/core/gc/gx_d3d11/hlsl_gen.hpp/.cpp` (`std::string generate_pixel_shader(const PixelKey&)`), and the shader cache in `d3d11_backend.cpp`
- Modify: the blend state (BP `0x41` BLENDMODE: blend factors, subtract, logic ops via emulation where D3D11 lacks them, color/alpha update, dst alpha `0x42`)
- Test: `tests/gc/test_pixel_key.cpp`, `tests/gc_d3d11/test_d3d11_tev.cpp`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: `gx::PixelShaderKey gx::make_pixel_key(const gx::State&)` (hashable, `operator==`), and `std::string gx::d3d11::generate_pixel_shader(const PixelShaderKey&)`.
- The TEV arithmetic follows Dolphin's `PixelShaderGen` integer emulation: 8-bit inputs with 10-bit signed intermediates, `(d + lerp(a, b, c) + bias) * scale` with the rounding as in Dolphin. Tests check exact values.

- [ ] **Step 1: Write the failing tests:** `TEST_CASE("TEV modulate, add, compare and konst give Dolphin's integer results")` (WARP, a 1×1 draw per case, about 12 cases from Dolphin's math); `TEST_CASE("alpha compare with AND/OR/XOR/XNOR discards")`; `TEST_CASE("blend and logic op results")`; `TEST_CASE("RGB565 and RGBA6 EFB formats quantise")`; `TEST_CASE("every generated shader compiles")`, generating keys over a seeded random sweep (200 keys) and compiling each on WARP.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the tests and expect PASS. Run the MKDD screenshot and update the progress doc.
- [ ] **Step 5: Commit.** Message: `GameCube GX: TEV, alpha test, blending, fog and EFB formats`

### Task 6: EFB copies to textures and CPU EFB access

**Files:**
- Create: `src/core/gc/gx/efb_encode.hpp/.cpp` (encode RGBA/Z readbacks into the texture copy formats, as in Dolphin's `TextureConversionShader`/`EncodeToRam`: the intensity formats, RGB565, RGB5A3, RGBA8, Z formats, with the box filter and half-size options)
- Modify: the BP `0x52` path (copy to texture: source rect, destination `0x4B`, stride `0x4D`, clear afterward). EFB CPU access `0x08000000` physical (`0xC8000000`) only if MKDD touches it, through `read_color/read_depth` and a new `write_pixel`; otherwise it remains an error.
- Test: `tests/gc/test_efb_encode.cpp`, `tests/gc_d3d11/test_d3d11_copy.cpp`

**Interfaces:**
- Consumes: Tasks 1–5.
- Produces: `void gx::encode_efb_copy(TextureCopyFormat, std::span<const std::uint32_t> rgba_or_z, int w, int h, bool half, std::span<std::uint8_t> dest, std::uint32_t stride)`. The texture cache from Task 4 picks up the new contents through its hash.

- [ ] **Step 1: Write the failing tests:** `TEST_CASE("texture copy encodes the format at the destination")` with each format on a small known image; `TEST_CASE("half-size copy box-filters 2x2")`; `TEST_CASE("WARP: render, copy to texture, draw with it")` as a round trip.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the tests and expect PASS.
- [ ] **Step 5: Commit.** Message: `GameCube GX: EFB copies to textures`

### Task 7: MKDD to its title screen

**Files:**
- Modify: whatever MKDD shows is missing. Each addition comes with its unit test.
- Create: `docs/progress/2026-11-24-gamecube-mkdd-title.md` (Spanish): each stop, what it needed, and the screenshots described.
- Modify: `pendientes/recordatorios.md`

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Run** `ezy_gc_runner "<MKDD>" --fields N --gx-warp --screenshot build/gc/mkdd_title_<N>.bmp` for growing N (600, 1200, 1800, 2400). For each stop: implement the missing behavior (hardware/Dolphin-cited only), add its test, re-run, and log it.
- [ ] **Step 2: Check the criterion** (spec goal 1). The screenshot at a field after the logos shows a picture that is neither black nor one color, and the progress doc describes it (logo, background, text). If after about 25 fixed stops the title is still not reached, stop with DONE_WITH_CONCERNS listing the remaining blockers. Don't hack around them.
- [ ] **Step 3: Run the full checks:** `ctest --preset debug` (all entries, foreground, nothing else running), and a clean `/W4` Release build of `ezyemu` and `ezy_gc_runner`.
- [ ] **Step 4: Update the notes** (`pendientes/recordatorios.md`: what works, next steps). The user confirms the title screen in ezyemu with the hardware device.
- [ ] **Step 5: Commit.** Message: `GameCube GX: Mario Kart: Double Dash!! reaches its title screen`
