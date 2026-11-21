# GameCube Sub-project 1 (Base and MKDD Boot) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A GameCube core (`ezy_gc`) whose interpreted Gekko CPU, memory, peripherals, disc and HLE boot bring Mario Kart: Double Dash!! from its disc image into its main loop, sending graphics commands every field. It comes with a headless runner and an ezyemu console.

**Architecture:** `src/core/gc` mirrors `src/core/n64`: a CPU (`gekko/`), a bus with one class per peripheral, a scheduler of absolute CPU cycles, a disc reader, a DOL loader, the HLE boot, and a graphics FIFO that parses and counts commands without drawing. The interpreter is the future JIT's reference, so every instruction is unit-tested.

**Tech Stack:** C++20, MSVC Build Tools 2022, CMake presets, doctest. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-21-gamecube-base-design.md`

## Global Constraints

- Branch `gc/base` (from `main`). Code, comments and commits in English; docs and notes in Spanish.
- Build only with presets: `cmake --preset default` (only when CMake files change and the build asks for it), `cmake --build --preset debug|release`, `ctest --preset debug`. CMake path: `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`. Builds are `/W4` clean.
- No new dependencies. Disc images, ROMs, BIOS and DSP microcode never go into the repo.
- Every unimplemented instruction, MMIO register or disc command stops the machine with an error that names the PC and the word or address. Nothing is silently ignored. Registers the spec lists as "accepted, no effect" are explicit no-ops, each with a comment.
- No per-game hacks (memory patches, address checks). Everything is hardware behavior or the documented HLE boot.
- CPU 486 MHz, 1 cycle per instruction. Bus 162 MHz. Time base and decrementer tick once every 12 CPU cycles (40.5 MHz). MEM1 is 24 MiB (`0x01800000`) at physical 0. ARAM is 16 MiB. Hardware registers start at physical `0x0C000000`: CP `+0x0000`, PE `+0x1000`, VI `+0x2000`, PI `+0x3000`, MI `+0x4000`, DSP `+0x5000`, DI `+0x6000`, SI `+0x6400`, EXI `+0x6800`, AI `+0x6C00`. The write-gather pipe is at `0x0C008000`. PVR = `0x00083214`.
- The existing suites keep passing (`ctest --preset debug`: today 11 entries; the new `ezy_gc_tests` adds one).
- MKDD: `E:/Rom nintendo/` (the file the user is downloading; check its name and format, see Task 7). Never commit it.
- Python/bash heredocs turn `\n` inside C++ string literals into real newlines, so use the Edit tool for C++ strings.

## Review Focus

1. A store that reaches a hardware register or the write-gather pipe must have the register's side effect (FIFO advance, DMA start, interrupt acknowledge), never land in RAM (tests in Tasks 4–6).
2. External interrupts must reach the CPU only when MSR[EE] is set, and `rfi` must restore MSR from SRR1 so pending interrupts are taken right after it (Task 1 test "external interrupt waits for MSR[EE]").
3. Single-precision FPU instructions and paired singles round to single precision at every step, and quantized loads and stores use the GQR type and scale exactly (Task 2 tests).
4. A DMA (DI, ARAM, cache DMA, EXI) must stay inside MEM1/ARAM. An out-of-range or misaligned request stops with an error and never writes past a buffer (tests in Tasks 3–5).
5. Graphics commands that span FIFO wrap-around and display lists nested in the FIFO must be sized correctly. A mis-sized primitive desynchronizes every command after it (Task 6 tests "command crosses the FIFO end" and "primitive sizes from VCD/VAT").

---

### Task 1: Gekko integer core, BAT translation, exceptions

**Files:**
- Create: `src/core/gc/CMakeLists.txt` (library `ezy_gc`, include dir `src/core`, linked to `ezy_warnings`); add `add_subdirectory(src/core/gc)` to the top `CMakeLists.txt` and `tests/gc` to the tests list (follow how `tests/n64` is added).
- Create: `src/core/gc/gekko/gekko.hpp`, `gekko.cpp` (state, step, exceptions, BAT translation), `gekko_integer.cpp` (integer, branch, load/store, CR, SPR, system ops), `cpu_bus.hpp` (the interface the CPU uses for physical memory).
- Test: `tests/gc/CMakeLists.txt` (`ezy_gc_tests`, doctest, `add_test`), `tests/gc/test_main.cpp`, `tests/gc/ppc_asm.hpp` (a small PowerPC encoder for tests, like `tests/n64/mips_asm.hpp`), `tests/gc/flat_bus.hpp`, `tests/gc/test_gekko_integer.cpp`, `tests/gc/test_gekko_system.cpp`.

**Interfaces:**
- Produces:
  - `struct CpuBus { virtual std::uint32_t read32(std::uint32_t paddr)=0; read16, read8, read64; write8/16/32/64(paddr, value); virtual ~CpuBus()=default; };`. Physical addresses; big-endian values in host order.
  - `class Gekko { explicit Gekko(CpuBus&); void reset(); int step(); /* returns the cycles used (1) */ void set_external_interrupt(bool level); void set_decrementer_tick(...)` (or `advance_timebase(std::uint64_t ticks)`, which also counts DEC down and raises its exception on a 0→negative transition); state accessors `gpr(i)`, `set_gpr`, `pc()`, `set_pc`, `msr()`, `set_msr`, `spr(n)`, `set_spr(n,v)`, `cr()`, `xer()`, `lr()`, `ctr()`; `fpr` accessors are added in Task 2.
  - `struct GekkoError : std::runtime_error { std::uint32_t pc, word_or_address; }` is thrown for unimplemented opcodes, unmapped addresses and unknown SPRs.
  - BAT translation: `std::optional<std::uint32_t> translate(std::uint32_t ea, bool instruction, bool write)`. With MSR[IR]/[DR] off it returns the identity. With them on it uses the 4 IBAT or 4 DBAT pairs; a miss raises ISI/DSI.
- Exceptions: system reset 0x100, DSI 0x300, ISI 0x400, external 0x500, alignment 0x600, program 0x700, FP unavailable 0x800, decrementer 0x900, syscall 0xC00. On entry: SRR0 = the faulting or next PC as the manual says, SRR1 = MSR bits (plus cause bits for program/DSI), MSR cleared except ME, IP selects base `0xFFF00000` vs `0`. DSI sets DAR/DSISR.

- [ ] **Step 1: Write the failing tests:**
  - `TEST_CASE("add, subf, addic and adde set CA, OV, SO and CR0")`: `0x7FFFFFFF + 1` with `addo.` gives OV=1, SO=1, CR0 = LT|SO; `addic` carries out of `0xFFFFFFFF + 1`; `subfe` uses CA.
  - `TEST_CASE("rlwinm, rlwimi and rlwnm masks wrap around")`: MB > ME masks, e.g. `rlwinm r3,r4,0,28,3`.
  - `TEST_CASE("divw and divwu by zero and overflow give the hardware results")`: divw by 0 → rD = 0 for nonnegative rA and -1 (`0xFFFFFFFF`) for negative rA; divw `0x80000000 / -1` → rD = -1 (Gekko behavior; cite the source in a comment); OV with `divwo`.
  - `TEST_CASE("branches use BO and BI, decrement CTR and set LR")`: bc with CTR decrement and condition, `bclr`, `bcctr`, `bl`.
  - `TEST_CASE("loads and stores: update forms, indexed, byte-reversed, lmw/stmw")`.
  - `TEST_CASE("BAT translation maps 0x80000000 and 0xC0000000 to physical 0")`, plus `TEST_CASE("an address no BAT covers raises DSI with DAR and DSISR")`.
  - `TEST_CASE("external interrupt waits for MSR[EE]")`: the level is set while EE=0, `rfi` to code with EE=1, and the interrupt is taken before that code's first instruction completes. SRR0 = that instruction's address.
  - `TEST_CASE("decrementer raises its exception on the 0 to negative transition")`.
  - `TEST_CASE("sc, rfi, mtmsr, mfspr and mtspr of HID, SPRG, BAT, DEC, TB")`.
  - `TEST_CASE("an unimplemented opcode throws GekkoError with the pc and the word")`.
- [ ] **Step 2: Run them to see them fail.** Run `cmake --preset default && cmake --build --preset debug --target ezy_gc_tests` and expect compile errors (no `gekko.hpp`).
- [ ] **Step 3: Implement** the decoder with tables by primary opcode (bits 0–5) and the extended opcodes of 19 and 31, plus the instructions the spec lists for the integer side (`lswi`/`stswi` included).
- [ ] **Step 4: Run the tests.** `ctest --preset debug -R ezy_gc_tests` should PASS, and the full `ctest --preset debug` should pass too.
- [ ] **Step 5: Commit.** Message: `GameCube: Gekko integer core with BAT translation and exceptions`

### Task 2: Gekko FPU and paired singles

**Files:**
- Create: `src/core/gc/gekko/gekko_fpu.cpp`, `src/core/gc/gekko/gekko_paired.cpp` (quantization included)
- Modify: `src/core/gc/gekko/gekko.hpp` (FPRs as `ps0`/`ps1` doubles, FPSCR, GQR accessors)
- Test: `tests/gc/test_gekko_fpu.cpp`, `tests/gc/test_gekko_paired.cpp`

**Interfaces:**
- Consumes: Task 1's `Gekko`, `CpuBus` and the decoder tables.
- Produces: `double ps0(int)`, `double ps1(int)`, `set_ps(int, double, double)`, `fpscr()`, `set_fpscr()`. Double ops set ps0 (on Gekko, ps1 is left undefined, and we leave it unchanged). Single ops (`fadds` and the rest) round to single and write both ps0 and ps1 (the Gekko duplicates the result). `lfs` duplicates into ps1. MSR[FP] = 0 raises FP unavailable.
- Quantized load/store: GQR[i] holds the store type and scale in bits 16–21/24–29 and the load type and scale in bits 0–5/8–13 (types 0 float, 4 u8, 5 u16, 6 s8, 7 s16; scale is signed 6-bit, value × 2^-scale on load and × 2^scale on store, saturating on store). `psq_l` with W=1 loads one value and sets ps1 = 1.0.

- [ ] **Step 1: Write the failing tests:**
  - `TEST_CASE("fadds rounds to single and duplicates into ps1")`.
  - `TEST_CASE("fmadd and fmadds fuse before rounding")`: use a case where a separate multiply-and-add would differ.
  - `TEST_CASE("frsp, fctiw and fctiwz follow the FPSCR rounding mode")`: all 4 RN modes; saturation of large values to `0x7FFFFFFF`/`0x80000000`.
  - `TEST_CASE("fcmpu and fcmpo set CR fields and FPCC, NaN unordered")`.
  - `TEST_CASE("mtfsf, mtfsfi, mtfsb0/1 and mffs")`.
  - `TEST_CASE("ps_add, ps_madd, ps_sum0/1, ps_muls0/1, ps_merge00..11 work lane by lane")`.
  - `TEST_CASE("psq_l and psq_st quantize u8, s8, u16, s16 with scale, saturating on store")`: e.g. a store of 300.0 as u8 with scale 0 gives 255; a load of s8 `0x80` with scale 1 gives -64.0.
  - `TEST_CASE("psq_l with W=1 sets ps1 to 1.0")`.
  - `TEST_CASE("an FPU instruction with MSR[FP]=0 raises FP unavailable")`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.** Use host doubles. Single rounding goes through `float` with the host rounding mode set from FPSCR[RN] (`std::fesetround`), restored after each op.
- [ ] **Step 4: Run the tests and see them pass.** Then run the full Debug suite.
- [ ] **Step 5: Commit.** Message: `GameCube: Gekko FPU and paired singles with GQR quantization`

### Task 3: Disc, DOL loader, HLE boot, memory map

**Files:**
- Create: `src/core/gc/disc.hpp/.cpp` (ISO/GCM reader by file path; streaming reads, never the whole file in memory), `src/core/gc/dol.hpp/.cpp`, `src/core/gc/hle_boot.hpp/.cpp`, `src/core/gc/memory.hpp/.cpp` (MEM1, locked cache, the physical dispatch to RAM / MMIO / pipe), `src/core/gc/scheduler.hpp/.cpp` (copy the N64 scheduler's design: a fixed array of event types and absolute cycles)
- Test: `tests/gc/test_disc_boot.cpp`, `tests/gc/disc_builder.hpp` (builds a synthetic disc in memory or a temp file: a header, a FST with one file, and a DOL assembled with `ppc_asm.hpp`)

**Interfaces:**
- Consumes: Tasks 1–2's `Gekko`, `CpuBus`.
- Produces:
  - `class Disc { static std::unique_ptr<Disc> open(const std::filesystem::path&); bool read(std::uint64_t offset, std::span<std::uint8_t> out); std::string game_id() const; std::uint32_t dol_offset() const; fst_offset(), fst_size(); }`. `open` returns nullptr (and says why on stderr) for files that are not a GCM: check the magic `0xC2339F3D` at `0x1C`.
  - `struct Dol { static std::optional<Dol> parse(std::span<const std::uint8_t>); entry; sections (offset, address, size); bss_address, bss_size; }`.
  - `void hle_boot(Gekko&, Memory&, Disc&)`: implements the spec's "Arranque HLE" section. The low-memory globals cite their source in code comments (YAGCD / Dolphin's `BootManager`/`Boot_BS2Emu` behavior). r1 = `0x816FFFFC`; r2/r13 are left to the DOL's crt0.
  - `Memory` implements `CpuBus` and dispatches physical ranges: MEM1, `0x0C000000` MMIO (to a `MmioHandler` interface the peripherals implement in Tasks 4–6, so this task can register a temporary handler that throws for every MMIO access), `0x0C008000` the pipe, `0xE0000000` the locked cache. Anything else throws `GekkoError`-style errors with the address.

- [ ] **Step 1: Write the failing tests:**
  - `TEST_CASE("Disc reads the header, game id, DOL and FST offsets")`.
  - `TEST_CASE("Disc rejects a file without the GameCube magic")`.
  - `TEST_CASE("Dol loads text and data sections and clears BSS")`.
  - `TEST_CASE("hle_boot sets the low-memory globals, BATs, MSR, r1 and pc")`: check `0x80000020` = `0x0D15EA5E`, `0x80000028` = `0x01800000`, `0x800000F8` = 162000000, `0x800000FC` = 486000000; the FST address at `0x80000038` points to a copy of the disc's FST; pc = the DOL entry.
  - `TEST_CASE("a boot of the synthetic disc runs its DOL to a known result")`: the DOL writes `0x600DF00D` to a fixed RAM address and loops.
  - `TEST_CASE("an access to an unmapped physical address throws with the address")`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the tests and see them pass.** Then run the full Debug suite.
- [ ] **Step 5: Commit.** Message: `GameCube: disc reader, DOL loader, HLE boot and the physical memory map`

### Task 4: The machine, PI, MI, VI and the XFB picture

**Files:**
- Create: `src/core/gc/gamecube.hpp/.cpp` (owns Memory, Gekko, Scheduler, peripherals; `run_field()`), `src/core/gc/pi.hpp/.cpp`, `src/core/gc/mi.hpp/.cpp`, `src/core/gc/vi.hpp/.cpp`, `src/core/gc/mmio.hpp` (the `MmioHandler` interface: `read16/32`, `write16/32` by offset; 8-bit accesses throw)
- Test: `tests/gc/test_pi_vi.cpp`

**Interfaces:**
- Consumes: Task 3's `Memory`, `Scheduler`, `hle_boot`.
- Produces:
  - `class GameCube { static std::unique_ptr<GameCube> create(const std::filesystem::path& disc); void run_field(); /* until the VI starts the next field */ Image xfb() const; /* ARGB, width from VI, height of the frame */ Stats stats() const; void set_pad(const PadState&); }`. `Stats` holds the instructions, the interrupts by PI cause, the DI reads, the FIFO bytes this field, the DSP mails, and the fields run. `PadState` holds the buttons (GameCube bit layout), the main stick, the C stick and the analog L/R.
  - `struct Image { int width, height; std::vector<std::uint32_t> argb; };`
  - PI: interrupt cause `0x0C003000` and mask `0x0C003004`; the CPU external interrupt line = (cause & mask) != 0. Cause bits: GPU error 0, RSW 1, DI 2, SI 3, EXI 4, AI 5, DSP 6, MI 7, VI 8, PE token 9, PE finish 10, CP 11, debug 12, HSP 13, RSW-state 16. Also reset code `0x0C003024` and the FIFO registers base/end/write pointer at `0x0C00300C/10/14`.
  - VI: NTSC 525 lines at 59.94 Hz, interlaced. Line counter and the 4 display interrupts with their enable/status bits, cleared by writing them. XFB top/bottom addresses. YUV 4:2:2 (Y0 U Y1 V) → ARGB with BT.601. The XFB picture is 640 wide × the frame height (the field lines doubled when progressive).

- [ ] **Step 1: Write the failing tests:**
  - `TEST_CASE("PI raises the CPU interrupt when an enabled cause is set and clears it on acknowledge")`.
  - `TEST_CASE("VI display interrupt fires at its line and clears when written")`.
  - `TEST_CASE("VI field timing gives 59.94 fields per second of CPU cycles")`.
  - `TEST_CASE("XFB YUV to ARGB matches BT.601 for black, white and a color bar")`.
  - `TEST_CASE("a store to a hardware register does not touch RAM")`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the tests and see them pass.** Then run the full Debug suite.
- [ ] **Step 5: Commit.** Message: `GameCube: machine loop, PI, MI, VI and the XFB picture`

### Task 5: DI, SI, EXI, AI and the DSP interface

**Files:**
- Create: `src/core/gc/di.hpp/.cpp`, `si.hpp/.cpp`, `exi.hpp/.cpp`, `ai.hpp/.cpp`, `dsp.hpp/.cpp` (the DSP interface registers, ARAM, the DMA engines, and the minimal DSP HLE)
- Test: `tests/gc/test_di.cpp`, `tests/gc/test_si_exi.cpp`, `tests/gc/test_dsp_ai.cpp`

**Interfaces:**
- Consumes: Task 4's `GameCube` wiring, `MmioHandler`, PI causes, and the `Scheduler`.
- Produces:
  - **DI**: the commands from the spec (Inquiry `0x12`, Read `0xA8` with DMA, Seek `0xAB`, Request Error `0xE0`, `0xE1/0xE2/0xE4` audio-streaming "off"). A read finishes after a fixed delay of 50 µs + bytes/3.1 MB/s, scheduled, then raises TCINT if enabled. Cover closed.
  - **SI**: channel 0 = standard pad (id `0x0900`, poll response from `PadState`), channels 1–3 no device. Immediate transfers, polling with its interrupt per VI line count.
  - **EXI**: channel 0 device 1 = RTC/SRAM (the RTC as seconds since 2000-01-01 from the host clock, the SRAM 64 bytes of defaults with valid checksums; cite the layout source), memory card slots empty, immediate and DMA transfers with their interrupts.
  - **AI**: control, volume, sample counter, interrupt (streaming off).
  - **DSP interface**: mailboxes, control register (reset, halt, DSP/ARAM/AI interrupt bits and masks), ARAM DMA both ways with its interrupt, AI DMA consuming 32-byte blocks at 32 kHz with its interrupt.
  - DSP HLE: answers the DSP ROM boot handshake and the SDK's microcode upload. After that, it logs every mail and answers what the microcode protocol needs. The protocol is discovered in Task 7 with MKDD; this task covers the boot handshake only, and documents it with its source.

- [ ] **Step 1: Write the failing tests:**
  - `TEST_CASE("DI Read DMAs the disc bytes to MEM1 and raises TCINT after its delay")`.
  - `TEST_CASE("DI rejects a DMA outside MEM1 with an error")`.
  - `TEST_CASE("SI reports a standard pad and its buttons")`.
  - `TEST_CASE("EXI RTC and SRAM reads return the clock and a valid SRAM")`.
  - `TEST_CASE("ARAM DMA copies both ways and raises its interrupt")`.
  - `TEST_CASE("AI DMA consumes blocks at 32 kHz and raises its interrupt")`.
  - `TEST_CASE("DSP HLE answers the boot handshake mails")`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the tests and see them pass.** Then run the full Debug suite.
- [ ] **Step 5: Commit.** Message: `GameCube: DI, SI, EXI, AI and the DSP interface`

### Task 6: Graphics FIFO without drawing

**Files:**
- Create: `src/core/gc/gx_fifo.hpp/.cpp` (the write-gather pipe, CP registers and FIFO, the command parser, display lists, PE tokens/finish, EFB copy clear, counters), `src/core/gc/gx_vertex.hpp/.cpp` (vertex sizes from VCD/VAT)
- Test: `tests/gc/test_gx_fifo.cpp`

**Interfaces:**
- Consumes: Task 4's PI (FIFO registers, PE causes) and `Memory` (the pipe range).
- Produces:
  - `GxFifo::write_pipe(std::uint32_t value, int bytes)`: a 32-byte gather buffer that flushes to the FIFO in RAM at the PI write pointer, wrapping from end to base.
  - The CP reads from its read pointer to the write pointer and parses: NOP `0x00`, `LOAD_CP_REG` `0x08` (1 + 1 + 4 bytes), `LOAD_XF_REG` `0x10` (1 + 4 + 4·n), `LOAD_INDX_A..D` `0x20/0x28/0x30/0x38` (1 + 4), `CALL_DL` `0x40` (1 + 4 + 4, which runs the list from RAM), `INVALIDATE_VTX_CACHE` `0x48` (1), `LOAD_BP_REG` `0x61` (1 + 4), and primitives `0x80–0xB8` (1 + 2 count + count × vertex size from VCD lo/hi and VAT group of the 3-bit format in the opcode). An incomplete command waits for more data, including across the FIFO wrap.
  - BP side effects: token (`0x47`, `0x48`) → PE token register and its interrupt; finish (`0x45`) → PE finish interrupt; EFB copy (`0x52`) with the clear bit writes the clear color (BP `0x4F/0x50`) to the XFB area the copy targets (destination address BP `0x4B`, stride `0x4D`, the copy rectangle `0x49/0x4A`), converted to YUV.
  - CP registers at `0x0C000000` (status, control, clear, FIFO base/end/high/low watermarks, read/write pointers, breakpoint), with the watermark interrupts.
  - Counters for each field: FIFO bytes, commands by type, primitives, vertices.

- [ ] **Step 1: Write the failing tests:**
  - `TEST_CASE("pipe writes reach the FIFO in 32-byte bursts and wrap from end to base")`.
  - `TEST_CASE("primitive sizes from VCD/VAT")`: direct and indexed position, normal, color, and texcoord formats; a triangle list of 3 vertices with a known layout gives the exact byte count.
  - `TEST_CASE("command crosses the FIFO end")`: a `LOAD_XF_REG` split by the wrap is parsed once, whole.
  - `TEST_CASE("CALL_DL parses a display list in RAM and returns")`.
  - `TEST_CASE("BP token and finish raise their PE interrupts")`.
  - `TEST_CASE("EFB copy with clear writes the clear color to the XFB as YUV")`.
  - `TEST_CASE("an unknown command opcode stops with its address")`.
- [ ] **Step 2: Run them to see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run the tests and see them pass.** Then run the full Debug suite.
- [ ] **Step 5: Commit.** Message: `GameCube: graphics FIFO, command parser and PE events without drawing`

### Task 7: Runner, ezyemu console, and booting MKDD

**Files:**
- Create: `tools/gc_runner/CMakeLists.txt`, `tools/gc_runner/main.cpp`; add to the top `CMakeLists.txt`
- Create: `src/frontend/gamecube_system.hpp/.cpp`; modify `src/frontend/main.cpp` and `src/frontend/CMakeLists.txt`. `.iso`/`.gcm` open by path: `main.cpp` must not read a 1.4 GB file into memory for these extensions.
- Create: `docs/progress/2026-11-21-gamecube-mkdd-boot.md` (Spanish): the log of what MKDD needed, and the final criterion run.
- Modify: `pendientes/recordatorios.md` (the GameCube section, the next sub-project, a log line)
- Modify: any `src/core/gc` file that MKDD shows needs more behavior (each addition with its unit test)

**Interfaces:**
- Consumes: everything above.
- Produces:
  - `ezy_gc_runner <disc> --fields N [--screenshot out.bmp] [--trace N]`: prints the stats per 60 fields and at the end. On a `GekkoError` or an MMIO/disc error it prints the PC, the word or address, and the last N (default 64) PCs executed, then exits with code 1.
  - `GameCubeSystem` for ezyemu. The pad mapping comes from the spec. The screen is 640×480 from `GameCube::xfb()`.
- The MKDD dump: find it in `E:/Rom nintendo/`. If it is a `.zip`/`.7z`, extract it next to the others with the tools on this PC (`tar -xf` handles zip). If it is RVZ/NKit/CISO, or not there yet, record that in the progress doc. Then validate the runner with the synthetic disc from Task 3, finish the rest of the task, and stop with status DONE_WITH_CONCERNS naming the missing dump. Do not download anything.

- [ ] **Step 1: Write the failing test** `TEST_CASE("the runner boots the synthetic disc and reports its stats")` in `tests/gc/test_runner.cpp` (it drives `GameCube` the way the runner does).
- [ ] **Step 2: Implement** the runner and the ezyemu system, and run the test until it passes.
- [ ] **Step 3: Boot MKDD.** Run `ezy_gc_runner "<MKDD>" --fields 600`. For each stop: implement the missing behavior (hardware or documented HLE boot only), add its unit test, re-run, and log it in the progress doc. The DSP microcode protocol is discovered here (see the spec). Ruling guidance: when a stop needs more than one sub-project's scope (for example real GX drawing to get past a hang), document it and stop the loop with DONE_WITH_CONCERNS rather than hacking around it.
- [ ] **Step 4: Check the criterion** (spec goal 1): for 600 consecutive fields, FIFO bytes > 0 every field, the VI interrupts arrive, and nothing stops. Save `--screenshot build/gc/mkdd_600.bmp` and describe it in the progress doc.
- [ ] **Step 5: Run the full checks.** `ctest --preset debug` passes (12 entries) and the Release build is `/W4` clean.
- [ ] **Step 6: Commit** (one or several commits while iterating). Final message: `GameCube: runner, ezyemu console and MKDD boot to its main loop`
