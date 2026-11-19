# Núcleo de JIT + entrada MIPS (Fases 1–3) — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** un JIT general (`ezy_jit`) con una entrada MIPS para el VR4300 que da exactamente el
mismo estado que el intérprete, activable con `--cpu-jit`, más la medición que decide la Fase 4.

**Architecture:** la entrada MIPS parte el código de kseg0 en bloques, los traduce a un IR
lineal (valores SSA, salidas con su propio código de volcado) y el backend lo compila con
`asmjit::x86::Compiler`, que asigna los registros del host. Las instrucciones sin efectos en
el bus que aún no son nativas se ejecutan "en el sitio" llamando al intérprete; las que pueden
programar eventos o cambiar el modo terminan el bloque. Un presupuesto de ciclos calculado por
`N64::step()` garantiza que entre bloques no cae ningún evento ni Compare.

**Tech Stack:** C++20, MSVC 2022, CMake presets, AsmJit (SHA `677a10bac2c24f7e068cf1f9e675bf51c7b534ea`), doctest.

**Spec:** `docs/superpowers/specs/2026-11-18-cpu-jit-core-design.md` (aprobado 2026-11-18).
Este plan cubre el perfil inicial y las Fases 1–3, más la entrada nativa de ALU, saltos y
memoria de kseg0. **La Fase 4 (enlace de bloques, caché de registros entre instrucciones,
FPU nativa) y la opcional Fase 5 tienen su propio plan**, escrito con el perfil de la Tarea 0
y las estadísticas de la Tarea 11.

## Global Constraints

- Compilar solo con `cmake --preset default`, `cmake --build --preset debug|release`, `ctest --preset debug`.
- Opción CMake `EZY_N64_CPU_JIT` (ON por defecto en hosts x64). Con OFF todo compila y las suites pasan sin AsmJit.
- `--cpu-jit` en `ezy_n64_runner`, `ezyemu` y `ezy_n64_tests`; apagado por defecto.
- Formato de estados sin cambios (`kStateVersion` sigue en 13); cargar un estado vacía el JIT.
- Solo se compila código buscado por kseg0 en modo kernel (`física = virtual & 0x1FFFFFFF`).
- Un bloque o tramo corre solo si `now + ciclos < limit`, con `limit = min(evento más próximo de todos los tipos, now + cycles_until_compare())` (estricto).
- Ninguna excepción de C++ cruza código nativo: las ayudas llamadas desde bloques son `noexcept`, capturan y guardan la excepción, y `Vr4300Jit::execute()` la relanza al volver.
- La memoria de código y los metadatos de bloques (salidas, palabras) solo se liberan al vaciar toda la caché, nunca un bloque suelto.
- Respuestas en español; código, comentarios, commits y PRs en inglés, sin líneas de atribución.
- Nunca se suben ROMs ni el ZIP de la raíz.

## Review Focus

1. **Fallo del hilo del RDP durante un bloque** (la ayuda de barrera relanza): debe llegar como excepción normal a quien llamó `N64::step()`, sin cuelgue ni corrupción. Prueba en la Tarea 10.
2. **Cargar un estado o reiniciar a mitad de partida con bloques compilados**: la continuación debe ser idéntica a la del intérprete. Prueba en la Tarea 6.
3. **Búfer de código agotado en plena partida**: vaciar la caché entre bloques sin tocar metadatos en uso. Prueba en la Tarea 3 (presupuesto pequeño) y en la Tarea 4 (diferencial con presupuesto pequeño).
4. **Almacenamiento que reescribe la siguiente instrucción del mismo bloque**: debe ejecutarse la instrucción nueva. Prueba en la Tarea 5.
5. **Salto en la última palabra de una página de 4 KiB o al final de RDRAM**: no compilar fuera de la página ni de la RAM. Prueba en la Tarea 4.

---

### Task 0: Perfil de partida

**Files:**
- Create: `docs/performance/2026-11-18-n64-cpu-profile.md`

- [ ] **Step 1: Compilar Release**

Run: `cmake --preset default` y `cmake --build --preset release`
Expected: build sin errores.

- [ ] **Step 2: Perfilar la carrera**

Run: `build/default/tools/n64_runner/Release/ezy_n64_runner.exe "E:/Rom nintendo/<MK64>.z64" --benchmark --time-from 1500 1900 --audio-hle --rdp-threaded --gfx-hle --profile build/cpu-profile.csv`
(el nombre exacto del archivo está en la memoria `reference-user-roms`; la sintaxis de `--benchmark` en `tools/n64_runner/main.cpp`.)
Expected: CSV con muestras por función.

- [ ] **Step 3: Escribir el documento**

Con el CSV: porcentaje del hilo principal en `Vr4300::step`/`execute*`/`load`/`store`/`translate*`/`Cop1`/`Bus`/scheduler/RSP. Indicar si `translate_mapped` o `Tlb::translate` superan el 2 %: si es así, la Fase 5 entra en el proyecto (spec, "No metas") y se anota en el documento. Incluir campos/s de esa corrida.

- [ ] **Step 4: Commit**

```bash
git add docs/performance/2026-11-18-n64-cpu-profile.md
git commit -m "N64: profile the race before the CPU JIT"
```

---

### Task 1: AsmJit compartido y biblioteca `ezy_jit` vacía

**Files:**
- Create: `cmake/asmjit.cmake`, `src/core/jit/CMakeLists.txt`, `src/core/jit/jit.cpp`, `tests/jit/CMakeLists.txt`, `tests/jit/test_main.cpp`, `tests/jit/test_build.cpp`
- Modify: `CMakeLists.txt` (raíz), `src/core/n64/CMakeLists.txt:13-51`

**Interfaces:**
- Produces: variable CMake `EZY_HOST_X64`; opciones `EZY_N64_RSP_JIT` y `EZY_N64_CPU_JIT` declaradas en `cmake/asmjit.cmake`; target `asmjit::asmjit` si alguna está ON y el host es x64; target `ezy_jit` (STATIC, `PUBLIC EZY_JIT_AVAILABLE=0|1`); ejecutable `ezy_jit_tests` registrado como test `ezy_jit_tests`.

- [ ] **Step 1: Test de humo**

```cpp
// tests/jit/test_build.cpp
TEST_CASE("ezy_jit links and reports whether native code is available") {
    CHECK(ezy::jit::available() == (EZY_JIT_AVAILABLE == 1));
}
```

- [ ] **Step 2: Mover la detección de x64 y la descarga de AsmJit a `cmake/asmjit.cmake`**, incluido desde la raíz justo después de `include(FetchContent)`. Mismo SHA y mismas cachés (`ASMJIT_STATIC ON`, `ASMJIT_TEST OFF`). `src/core/n64/CMakeLists.txt` conserva solo `target_link_libraries(ezy_n64 PRIVATE asmjit::asmjit)` y la definición `EZY_N64_RSP_JIT` según la opción.

- [ ] **Step 3: Crear `ezy_jit`** con `bool ezy::jit::available()` (true si se compiló con AsmJit), enlazando AsmJit PRIVATE, y `add_subdirectory(src/core/jit)` antes de `src/core/n64`; `tests/jit` dentro del bloque de tests.

- [ ] **Step 4: Verificar las tres combinaciones**

Run: `cmake --preset default -DEZY_N64_RSP_JIT=OFF -DEZY_N64_CPU_JIT=ON`, build debug, `ctest --preset debug` → todas pasan y `ezy_jit_tests` informa disponible.
Run: lo mismo con `-DEZY_N64_CPU_JIT=OFF -DEZY_N64_RSP_JIT=OFF` → todo pasa, AsmJit no se descarga.
Run: volver a `-DEZY_N64_RSP_JIT=ON -DEZY_N64_CPU_JIT=ON` (los valores por defecto) y repetir.
Expected: 9 suites en verde en cada caso (las 8 de hoy + `ezy_jit_tests`; con RSP JIT OFF desaparecen las 2 del RSP JIT).

- [ ] **Step 5: Commit** — `Build: share AsmJit between the RSP and CPU JITs; add ezy_jit`

---

### Task 2: IR y backend x86-64

**Files:**
- Create: `src/core/jit/context.hpp`, `src/core/jit/ir.hpp`, `src/core/jit/ir.cpp`, `src/core/jit/x64_backend.hpp`, `src/core/jit/x64_backend.cpp`, `src/core/jit/code_buffer.hpp`, `src/core/jit/code_buffer.cpp`
- Test: `tests/jit/test_backend.cpp`

**Interfaces:**
- Produces (todo en `namespace ezy::jit`):

```cpp
// context.hpp — lo que todo bloque recibe en su único argumento.
struct PageBarrierView {            // fast path of RdpWorker::wait_memory, read-only
    const std::uint16_t* writers;   // slot per 4 KiB page
    const std::uint16_t* readers;
    const std::uint64_t* slot_sequence;
    const std::uint64_t* completed_seen;
    const std::atomic<bool>* failed;
};
struct Context {
    std::uint8_t* guest;                 // base the front-end's guest offsets are relative to
    std::uint8_t* ram;                   // big-endian RAM, physical address 0
    std::uint32_t ram_size;
    const std::uint8_t* code_pages;      // CodePages::table(), one byte per 4 KiB of ram
    const PageBarrierView* barrier;      // nullptr: no barrier
    std::int64_t budget;                 // cycles the current block may still use (strict)
    std::int64_t extra_cycles;           // dynamic cycles added by helpers in this block
    void* user;                          // the front-end (Vr4300Jit*)
    // Slow paths the backend calls; all noexcept, set by the front-end.
    void (*barrier_wait)(Context*, std::uint32_t paddr, std::uint32_t bytes, std::uint32_t write) noexcept;
    std::uint32_t (*code_write)(Context*, std::uint32_t paddr, std::uint32_t bytes) noexcept;  // 1 = compiled code changed
};
enum class ContextField : std::uint8_t { Budget, ExtraCycles };

// ir.hpp
enum class Type : std::uint8_t { I32, I64, F32, F64, F32x2 };  // floats: rejected by compile() until the Phase 4 plan
enum class Cond : std::uint8_t { Eq, Ne, LtS, LtU, LeS, GtS, GeS, GeU };
enum class Op : std::uint8_t {
    Const, LoadGuest, StoreGuest, LoadContext, StoreContext,
    Add, Sub, And, Or, Xor, Nor, Shl, Shr, Sar, Mul, MulHiS, MulHiU,
    Sext8, Sext16, Sext32, Zext32, Trunc,           // to the result type
    Compare, Select, OverflowAdd, OverflowSub, CarryAdd,
    LoadRam, StoreRam, Barrier, NoteRamWrite, Call,
    ExitIf, Exit,
};
struct Value { std::uint32_t id = ~0U; };
using ExitId = std::uint32_t;
using Helper = std::uint64_t (*)(Context*, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;

class IrBlock;  // ops + exits; built only through IrBuilder
class IrBuilder {
public:
    Value constant(Type type, std::uint64_t value);
    Value load_guest(Type type, std::uint32_t offset);
    void store_guest(std::uint32_t offset, Value value);
    Value load_context(ContextField field);                 // I64
    void store_context(ContextField field, Value value);
    Value binary(Op op, Value a, Value b);                  // same type; shifts mask the amount by width-1
    Value convert(Op op, Type to, Value v);                 // Sext8/16/32, Zext32, Trunc
    Value compare(Cond cond, Value a, Value b);             // I32 0/1
    Value select(Value cond, Value if_true, Value if_false);
    Value overflow(Op add_or_sub, Value a, Value b);        // I32 0/1, signed, at the operands' width
    Value carry_add(Value a, Value b);                      // I32 0/1, unsigned carry out
    Value load_ram(Type type, Value paddr, unsigned bytes); // big-endian, zero-extended; paddr is I32, checked by the front-end
    void store_ram(Value paddr, Value value, unsigned bytes);
    void barrier(Value paddr, unsigned bytes, bool write);  // inline fast check, ctx->barrier_wait on the slow path
    Value note_ram_write(Value paddr, unsigned bytes);      // I32: 1 if compiled code changed
    Value call(Helper helper, Value a, Value b, Value c);   // I64; use constant(I64, 0) for unused arguments
    ExitId add_exit(std::uint32_t tag);                     // tag: front-end meaning, returned by the block
    void begin_exit(ExitId exit);                           // ops until end_exit() run only on that exit
    void end_exit();
    void exit_if(Value cond, ExitId exit);
    void exit(ExitId exit);                                 // last op of the main body
    [[nodiscard]] IrBlock finish();                         // verifies; throws std::logic_error on misuse
};

// code_buffer.hpp / x64_backend.hpp
using BlockFn = std::uint32_t (*)(Context*) noexcept;      // returns the exit's tag
class CodeBuffer {
public:
    explicit CodeBuffer(std::size_t budget_bytes);
    [[nodiscard]] bool available() const;
    [[nodiscard]] std::size_t used() const;
    void reset();                                           // hard reset (ResetPolicy::kHard), never from native code
    [[nodiscard]] asmjit::JitRuntime& runtime();
};
enum class CompileError : std::uint8_t { None, OutOfSpace, Unsupported };
struct Compiled { BlockFn fn = nullptr; CompileError error = CompileError::None; };
[[nodiscard]] Compiled compile(const IrBlock& block, CodeBuffer& buffer);
```

- [ ] **Step 1: Tests que fallan** (todos con un `Context` sobre arrays locales; `guest` = `std::array<std::uint64_t, 8>`):

```cpp
TEST_CASE("IR adds two guest registers and stores the sign-extended 32-bit sum") {
    // guest[1] = 0x7FFFFFFF, guest[2] = 1 -> guest[0] = Sext32(Trunc(g1) + Trunc(g2)) = 0xFFFFFFFF80000000
    CHECK(guest[0] == 0xFFFFFFFF80000000ULL); CHECK(tag == 7);
}
TEST_CASE("IR exit_if takes the side exit and runs only its own stores") {
    // cond true: guest[3] = 11 written only inside begin_exit(e1); main body writes guest[4] = 22 after exit_if
    CHECK(tag == 1); CHECK(guest[3] == 11); CHECK(guest[4] == 0);
}
TEST_CASE("IR 64-bit ops match C++ for edge values") {
    // for a, b in {0, 1, ~0, 1<<63, 0x8000'0000, 0x7FFF'FFFF'FFFF'FFFF}: Add Sub And Or Xor Nor Shl/Shr/Sar (b&63) Mul MulHiS MulHiU
    CHECK(out == expected);  // expected computed with _mul128/_umul128 and plain C++
}
TEST_CASE("IR compare, select, overflow and carry give 0 or 1") {
    CHECK(overflow(Add, I32, 0x7FFFFFFF, 1) == 1); CHECK(overflow(Sub, I64, 1ULL<<63, 1) == 1);
    CHECK(carry_add(I32, 0xFFFFFFFF, 1) == 1); CHECK(compare(LtU, I64, 1, ~0ULL) == 1);
}
TEST_CASE("IR RAM loads and stores are big-endian") {
    // ram = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88}
    CHECK(load_ram(I32, 0, 4) == 0x11223344); CHECK(load_ram(I64, 0, 8) == 0x1122334455667788ULL);
    CHECK(load_ram(I32, 6, 2) == 0x7788);      // after store_ram(0, 0xAABB, 2): ram[0..1] == {0xAA, 0xBB}
}
TEST_CASE("IR calls a helper with the context and three arguments") { CHECK(result == 0x10203); }
TEST_CASE("IR barrier calls barrier_wait only when the page's packet is newer than completed_seen") {
    CHECK(waits == 0); /* writers[0]=1, slot_sequence[1]=5, completed_seen=5 */
    CHECK(waits == 1); /* completed_seen=4 */ CHECK(waits_with_failed_set == 1);
}
TEST_CASE("IR note_ram_write calls code_write only for marked pages and returns its answer") {
    CHECK(calls == 0); CHECK(changed == 0); /* page unmarked */ CHECK(calls == 1); CHECK(changed == 1);
}
TEST_CASE("IR rejects float types and misuse") { CHECK_THROWS_AS(builder.constant(Type::F32, 0), std::logic_error); }
TEST_CASE("compile reports OutOfSpace on a tiny buffer") { CHECK(compile(big_block, CodeBuffer(64)).error == CompileError::OutOfSpace); }
```

- [ ] **Step 2: Ejecutar y ver que fallan** — `ctest --preset debug -R ezy_jit_tests` → FAIL (no compilan).

- [ ] **Step 3: Implementar** `IrBuilder` (vector de ops con tipo, operandos e inmediato; regiones de salida guardadas aparte; `finish()` verifica tipos, que cada salida tenga `end_exit`, y que el cuerpo termine en `exit`) y `compile()` con `asmjit::x86::Compiler`: un `x86::Gp` virtual por valor, el `Context*` en un registro virtual fijado como argumento, cada región de salida como etiqueta al final que ejecuta sus ops y devuelve su tag. `Barrier` y `NoteRamWrite` en línea según las firmas de `Context`. Patrón de `CodeHolder`/`JitRuntime::add` como en `src/core/n64/rsp_jit.cpp:78-90`.

- [ ] **Step 4: Ejecutar** — `ctest --preset debug -R ezy_jit_tests` → PASS.

- [ ] **Step 5: Commit** — `JIT: linear IR and x86-64 backend`

---

### Task 3: Caché de bloques y páginas con código

**Files:**
- Create: `src/core/jit/code_pages.hpp`, `src/core/jit/block_cache.hpp`, `src/core/jit/block_cache.cpp`
- Test: `tests/jit/test_block_cache.cpp`

**Interfaces:**
- Consumes: `BlockFn`, `CodeBuffer` (Tarea 2).
- Produces:

```cpp
class CodePages {
public:
    explicit CodePages(std::uint32_t ram_size);               // ram_size / 4096 bytes, all 0
    [[nodiscard]] const std::uint8_t* table() const;
    void mark(std::uint32_t page);
    void clear(std::uint32_t page);
    [[nodiscard]] bool marked(std::uint32_t begin, std::uint32_t end) const;  // any page in [begin, end)
};
struct BlockExit { std::uint32_t cycles, instructions, kind; };  // static counts up to this exit; kind: front-end meaning
struct Block {
    BlockFn fn = nullptr;                // nullptr: "the interpreter runs this pc" (cached so it is not recompiled)
    std::uint32_t key = 0;               // mode key it was compiled for
    std::uint32_t paddr = 0, words = 0;  // never crosses a 4 KiB page
    std::uint32_t min_cycles = 0;        // static cycles of the whole block
    std::span<const BlockExit> exits;    // indexed by the tag BlockFn returns
    std::span<const std::uint32_t> code; // the words compiled (for the check mode)
};
class BlockCache {
public:
    BlockCache(std::uint32_t ram_size, std::size_t code_budget);
    [[nodiscard]] const Block* find(std::uint32_t paddr, std::uint32_t key) const;  // nullptr if absent or other key
    // Copies exits and code into the arena (freed only by flush), marks the page.
    const Block& insert(const Block& block, std::span<const BlockExit> exits, std::span<const std::uint32_t> code);
    bool invalidate(std::uint32_t begin, std::uint32_t end);  // drops the pages' blocks; true if any existed
    void flush();                                              // everything, including the code buffer
    [[nodiscard]] CodeBuffer& code();
    [[nodiscard]] const CodePages& pages() const;
    struct Stats { std::uint64_t blocks = 0, invalidations = 0, flushes = 0; };
    [[nodiscard]] Stats stats() const;
};
```

- [ ] **Step 1: Tests que fallan**

```cpp
TEST_CASE("BlockCache finds a block by physical address and mode key only") {
    CHECK(cache.find(0x1000, 1) != nullptr); CHECK(cache.find(0x1000, 2) == nullptr); CHECK(cache.find(0x1004, 1) == nullptr);
}
TEST_CASE("BlockCache invalidate drops every block of the touched pages and clears their marks") {
    CHECK(cache.invalidate(0x1FFC, 0x2000)); CHECK(cache.find(0x1000, 1) == nullptr);
    CHECK(cache.find(0x2000, 1) != nullptr); CHECK(!cache.pages().marked(0x1000, 0x2000));
}
TEST_CASE("BlockCache keeps a dropped block's exits readable until flush") {
    const Block& b = cache.insert(...); cache.invalidate(0x1000, 0x1004);
    CHECK(b.exits[0].cycles == 3);  // arena memory still valid
}
TEST_CASE("BlockCache flush empties the cache and the code buffer") { CHECK(cache.code().used() == 0); CHECK(cache.stats().flushes == 1); }
```

- [ ] **Step 2: Ejecutar y ver que fallan.**
- [ ] **Step 3: Implementar** tabla de dos niveles (por página de 4 KiB, 1024 ranuras reservadas a demanda); arena de metadatos en bloques grandes (`std::vector<std::unique_ptr<std::byte[]>>`). Un bloque con otra clave en la misma dirección se reemplaza.
- [ ] **Step 4: Ejecutar** → PASS.
- [ ] **Step 5: Commit** — `JIT: block cache with page-granular invalidation`

---

### Task 4: `Vr4300Jit` — bloques "en el sitio" y despachador

Todo se ejecuta todavía con el intérprete, pero sin decodificar ni retirar instrucción por
instrucción: es la base correcta sobre la que las Tareas 7–9 vuelven nativas las instrucciones.

**Files:**
- Create: `src/core/n64/cpu/vr4300_jit.hpp`, `src/core/n64/cpu/vr4300_jit.cpp`, `src/core/n64/cpu/vr4300_jit_decode.cpp`
- Modify: `src/core/n64/cpu/vr4300.hpp` (friend + dos métodos), `src/core/n64/cpu/vr4300.cpp`, `src/core/n64/CMakeLists.txt`
- Test: `tests/n64/test_cpu_jit.cpp`, `tests/n64/CMakeLists.txt`

**Interfaces:**
- Consumes: Tareas 2 y 3.
- Produces:

```cpp
// vr4300.hpp (public, for the JIT)
friend class Vr4300Jit;
// HostError: a C++ exception (e.g. an RDP worker failure) was caught and stored; exit at once.
enum class InPlace : std::uint8_t { Ok, Exception, ExitBefore, CodeChanged, HostError };
// step() without the interrupt check, fetch and retire: current_pc_ = pc_, in_delay_slot_ =
// branch_taken_, pc_ = next_pc_, next_pc_ += 4, execute(instr), gpr_[0] = 0.
InPlace execute_in_place(std::uint32_t instr, int& cycles);
// Loads/stores: ExitBefore (nothing changed) unless kernel mode and the address is kseg0/kseg1
// inside direct_ram(); then like execute_in_place (kseg1 adds its uncached cycles).
InPlace execute_memory_in_place(std::uint32_t instr, int& cycles);

// vr4300_jit.hpp
enum class Kind : std::uint8_t { Native, InPlace, Memory, OutOfBlock };
[[nodiscard]] Kind classify(std::uint32_t instr);      // vr4300_jit_decode.cpp
[[nodiscard]] bool is_branch(std::uint32_t instr);     // J JAL JR JALR, B*, B*L, REGIMM branches, BC1*
class Vr4300Jit {
public:
    Vr4300Jit(Vr4300& cpu, CpuBus& bus, std::size_t code_budget = 32 * 1024 * 1024);
    [[nodiscard]] bool available() const;
    struct Result { std::uint64_t cycles = 0, instructions = 0; };
    // Runs blocks from cpu.pc() while each fits strictly below min(event_budget, cycles_until_compare()).
    // Retires Count/Random (retire_many) itself. instructions == 0: the caller runs cpu.step().
    Result execute(std::uint64_t event_budget);
    void invalidate_ram(std::uint32_t begin, std::uint32_t end);  // from any RDRAM writer
    void flush();
    void set_check(bool enabled);                                  // Task 5
    struct Stats { std::uint64_t compiled = 0, in_place = 0, native = 0, out_of_block = 0,
                   exits_before = 0, compile_ns = 0, invalidations = 0, flushes = 0; };
    [[nodiscard]] Stats stats() const;
};
inline constexpr unsigned kMaxBlockInstructions = 64;
```

Reglas (del spec): `OutOfBlock` = COP0 (op 0x10); `Memory` = cargas/almacenamientos (incl. LL/SC, COP1); el resto `InPlace` (incluidos saltos, `SYSCALL`, `BREAK`, trampas, `CACHE`, COP1). El bloque termina tras salto + ranura, antes de un `OutOfBlock`, a las 64 instrucciones, o al final de la página de 4 KiB; un salto cuya ranura no cabría en la página, o cuya ranura es otro salto u `OutOfBlock`, termina el bloque antes del salto. Una dirección de inicio con bloque vacío se guarda con `fn == nullptr`. `execute()` no entra si `cpu.branch_taken_`, `cpu.interrupt_pending()`, fuera de modo kernel o fuera de kseg0/RDRAM. Tras cada instrucción con ciclos extra: si `ciclos_hechos + extra + ciclos_estáticos_restantes >= budget`, salida tras ella. `Exception` y `CodeChanged` salen tras la instrucción (cuenta como retirada); `ExitBefore` sale antes. El bucle de `execute()` sigue al siguiente bloque solo tras una salida normal al final del bloque. Clave de modo: `allow_64bit_ops()`, `CU1`, `FR`.

- [ ] **Step 1: Tests que fallan** — un arnés `Twin` con dos `FlatBus`+`Vr4300` iguales; uno avanza con `step()`, el otro con `jit.execute(budget)` o `step()` si devuelve 0, el mismo número de instrucciones; compara `StateWriter` de ambas CPU y la RAM:

```cpp
TEST_CASE("CPU JIT matches the interpreter on generated integer programs") {
    for (unsigned seed = 0; seed < 200; ++seed) { Twin t(seed); t.run_until_pc(kEnd); CHECK(t.same()); }
}
TEST_CASE("CPU JIT matches on branches, likely branches not taken and delay slots") { CHECK(t.same()); }
TEST_CASE("CPU JIT matches when an instruction in a block raises an exception") {
    // ADD overflow, unaligned LW, TEQ, SYSCALL in a delay slot (EPC and Cause.BD)
    CHECK(t.same()); CHECK(t.jit_cpu.cop0().read(Cop0::kEpc) == expected_epc);
}
TEST_CASE("CPU JIT stops before a block that does not fit the budget") { CHECK(jit.execute(3).instructions == 0); }
TEST_CASE("CPU JIT stops before Count reaches Compare") { CHECK(t.same()); CHECK(cause_ip7_set_at_same_instruction); }
TEST_CASE("CPU JIT leaves COP0 and MMIO to the interpreter") { CHECK(jit.stats().out_of_block > 0); CHECK(t.same()); }
TEST_CASE("CPU JIT does not compile past a page end or past RAM") {
    // branch at 0x1FFC with its slot at 0x2000; code running up to direct_ram_size()
    CHECK(t.same());
}
TEST_CASE("CPU JIT matches with a tiny code buffer that flushes mid-run") {  // Review Focus 3
    Twin t(7, /*code_budget=*/64 * 1024); t.run_until_pc(kEnd); CHECK(t.same()); CHECK(t.jit.stats().flushes > 0);
}
TEST_CASE("CPU JIT keeps idle loop detection working") { CHECK(t.jit_cpu.idle_loop_cycles() == 2); }
```

El generador: instrucciones de ALU de 32/64 bits, `LUI`, `MULT/DIV` y familia, saltos solo hacia delante, cargas/almacenamientos alineados a una zona de datos en kseg0, terminando en `kEnd`.

- [ ] **Step 2: Ejecutar** `ctest --preset debug -R ezy_n64_tests` → FAIL.
- [ ] **Step 3: Implementar** `classify`/`is_branch`, `execute_in_place`/`execute_memory_in_place`, y `Vr4300Jit` que construye el IR: por instrucción, `call(helper, instr)` y `exit_if` según el estado devuelto (las salidas llevan `BlockExit{ciclos, instrucciones, kind}`), sumando a `ExtraCycles`. Las ayudas capturan cualquier excepción de C++ en un `std::exception_ptr` de `Vr4300Jit` y devuelven `HostError`; `execute()` la relanza tras volver del bloque. Antes de leer las palabras de un bloque para compilarlo se llama `bus.guard_memory(paddr, bytes, false)` sobre su rango, igual que la búsqueda del intérprete (el RDP en hilos puede tener escrituras pendientes ahí). `Context::guest` = `reinterpret_cast<std::uint8_t*>(&cpu)`; los offsets de los miembros se calculan en el constructor como diferencias de punteros (no `offsetof`: `Vr4300` no es standard-layout).
- [ ] **Step 4: Ejecutar** → PASS con y sin `--rdp-threaded`.
- [ ] **Step 5: Commit** — `N64: CPU JIT dispatcher with in-place blocks`

---

### Task 5: Invalidación por todos los escritores de RDRAM y modo de comprobación

**Files:**
- Modify: `src/core/n64/cpu/cpu_bus.hpp`, `src/core/n64/cpu/vr4300.cpp` (`store`), `src/core/n64/bus.hpp`, `src/core/n64/bus.cpp`, `src/core/n64/rdp.hpp`, `src/core/n64/rdp_commands.cpp`, `src/core/n64/gfx_hle/f3dex.cpp` (solo si escribe RDRAM fuera del RDP), `src/core/n64/cpu/vr4300_jit.cpp`
- Test: `tests/n64/test_cpu_jit.cpp`, `tests/n64/test_bus.cpp`

**Interfaces:**
- Produces:

```cpp
// cpu_bus.hpp
using CodeWriteFn = void (*)(void* user, std::uint32_t begin, std::uint32_t end);
void set_code_watch(const std::uint8_t* pages, CodeWriteFn fn, void* user);  // pages == nullptr: off
void note_ram_write(std::uint32_t paddr, std::size_t bytes) const;          // inline page check, then fn
// rdp.hpp
void set_ram_write_listener(std::function<void(std::uint32_t begin, std::uint32_t end)> listener);
```

Escritores (todos llaman `note_ram_write` con el rango ya recortado a RDRAM): `Vr4300::store` (camino directo), `Bus::prepare_pi_dma` cuando `reg == 0x0C`, el SI cuando escribe RDRAM (`bus.cpp:93`), la fila de DMA de SP con `row.write`, el callback del audio HLE con `write`, y el RDP: síncrono y en hilos, en el hilo principal, con los rangos `write` del `RdpAccess` de cada paquete (que ya existen para las reservas). `Vr4300Jit` registra `set_code_watch` con `cache.pages().table()`; su función llama `invalidate_ram` y, si se invalidó el bloque en curso, la ayuda devuelve `CodeChanged`. `flush()` al cargar un estado (Tarea 6). Con `set_check(true)`, antes de entrar a cada bloque se comparan `Block::code` con la RAM; si difieren, `std::runtime_error` con la dirección física en hexadecimal.

- [ ] **Step 1: Tests que fallan**

```cpp
TEST_CASE("CPU JIT runs the new instruction after a store rewrites the next one in the same block") {  // Review Focus 4
    // block: sw t1 -> 0x1008 (writes "addiu t2, 0, 99"); 0x1008 was "addiu t2, 0, 1"
    CHECK(t.jit_cpu.gpr(t2) == 99); CHECK(t.same());
}
TEST_CASE("CPU JIT recompiles code overwritten by another block's store") { CHECK(t.same()); CHECK(jit.stats().invalidations >= 1); }
TEST_CASE("Bus reports PI, SI, SP DMA, audio HLE and RDP writes to the code watch") {
    // one sub-case per writer: CHECK(watch.ranges contains [begin, end))
}
TEST_CASE("CPU JIT check mode reports code changed behind its back") {
    // mutate FlatBus memory directly without note_ram_write
    CHECK_THROWS_WITH_AS(jit.execute(1000), doctest::Contains("0x00001000"), std::runtime_error);
}
```

- [ ] **Step 2: Ejecutar** → FAIL.
- [ ] **Step 3: Implementar** según las interfaces.
- [ ] **Step 4: Ejecutar** las suites N64 con y sin `--rdp-threaded` → PASS.
- [ ] **Step 5: Commit** — `N64: invalidate compiled CPU code on every RDRAM writer`

---

### Task 6: Integración en `N64`, runner y ezyemu; validación con juegos

**Files:**
- Modify: `src/core/n64/n64.hpp`, `src/core/n64/n64.cpp`, `tests/n64/test_main.cpp`, `tests/n64/CMakeLists.txt`, `tools/n64_runner/main.cpp`, `src/frontend/main.cpp`, `src/frontend/n64_system.hpp`, `src/frontend/n64_system.cpp`
- Test: `tests/n64/test_cpu_jit.cpp`, `tests/n64/test_state.cpp`

**Interfaces:**
- Produces:

```cpp
enum class CpuMode { Interpreter, Jit };
CpuMode default_cpu_mode(); void set_default_cpu_mode(CpuMode);
// N64
bool set_cpu_mode(CpuMode mode);            // false (and stays Interpreter) if built without the JIT
[[nodiscard]] CpuMode cpu_mode() const;
[[nodiscard]] const Vr4300Jit* cpu_jit() const;  // nullptr in interpreter mode
```

`N64::step()`: tras el salto de bucles ociosos, si hay JIT: `budget = scheduler_.earliest_except(0) - now`, `r = cpu_jit_->execute(budget)`; con `r.instructions != 0` → `scheduler_.advance(r.cycles)`, `run_due_events(false)`, return; si no, el camino de hoy. `load_state()` y la creación llaman `flush()`. `N64::create` aplica `default_cpu_mode()`. Runner: `--cpu-jit`, `--cpu-jit-check`, estadísticas del JIT en `--benchmark`; `--state-check` cruza también el modo de CPU (JIT ↔ intérprete). ezyemu: `--cpu-jit`, conservado en reset como `gfx_hle_`. CTest: `ezy_n64_cpu_jit_tests` (`--cpu-jit`) y `ezy_n64_cpu_jit_threaded_tests` (`--cpu-jit --rdp-threaded`), timeout 180.

- [ ] **Step 1: Tests que fallan**

```cpp
TEST_CASE("N64 with the CPU JIT loads a state and continues exactly like the interpreter") {  // Review Focus 2
    // test ROM from n64_test_helpers.hpp: run 2 frames, save, run 1, load, run 3 -> same state bytes as interpreter
    CHECK(jit_state == interpreter_state);
}
TEST_CASE("N64::set_cpu_mode refuses the JIT when it was not built") { CHECK(n64->set_cpu_mode(CpuMode::Jit) == (EZY_JIT_AVAILABLE == 1)); }
```

- [ ] **Step 2: Ejecutar** → FAIL. **Step 3: Implementar.** **Step 4:** `ctest --preset debug` → todas en verde (incluidas las dos nuevas); repetir con `-DEZY_N64_CPU_JIT=OFF` (las dos nuevas no se registran y el resto pasa).

- [ ] **Step 5: Validar con juegos (Release)** — con `--cpu-jit --cpu-jit-check` y luego sin `--cpu-jit-check`:
  - MK64 `--dump-state <out> 1900` idéntico al intérprete en las cuatro combinaciones RDP síncrono/hilos × con/sin `--gfx-hle` (comparar los archivos con `cmp`).
  - `--state-check 1700` en ambas direcciones (arrancando con y sin `--cpu-jit`), con y sin `--gfx-hle`.
  - `--frames`/`--compare` de MK64 a 120/300/600 campos y de Spider-Man: imágenes iguales al intérprete.
  - n64-systemtest con `--cpu-jit`: 2984/3721.
  Expected: todo igual. Cualquier diferencia se depura con superpowers:systematic-debugging antes de seguir.

- [ ] **Step 6: Commit** — `N64: --cpu-jit in the machine, runner, tests and ezyemu`

---

### Task 7: ALU entera nativa

**Files:**
- Modify: `src/core/n64/cpu/vr4300_jit_decode.cpp`, `src/core/n64/cpu/vr4300_jit.cpp` (o un nuevo `vr4300_jit_alu.cpp` si pasa de ~400 líneas)
- Test: `tests/n64/test_cpu_jit.cpp`

`Native`: `ADDIU ADDU SUBU DADDIU DADDU DSUBU ANDI ORI XORI LUI AND OR XOR NOR SLT SLTU SLTI SLTIU`, desplazamientos de 32 y 64 bits (`SLL SRL SRA SLLV SRLV SRAV DSLL DSRL DSRA DSLL32 DSRL32 DSRA32 DSLLV DSRLV DSRAV`), `MFHI MFLO MTHI MTLO`, `MULT MULTU DMULT DMULTU` (ciclos extra fijos 5 y 8), y `ADD ADDI SUB DADD DADDI DSUB` con `overflow` → salida *antes* (el intérprete levanta `Ov`). Las de 64 bits solo nativas si la clave dice `allow_64bit_ops()`; si no, `InPlace`. `DIV/DDIV` siguen `InPlace` (división por cero y casos límite del intérprete). Destino `$0`: no se escribe. El estado de PC (`pc_`, `next_pc_`, `branch_taken_`) se escribe antes de cada llamada a ayuda y en cada salida, no por instrucción nativa.

- [ ] **Step 1: Test que falla**

```cpp
TEST_CASE("CPU JIT runs integer ALU natively and still matches") {
    // same generator as Task 4, 200 seeds, plus edge values {0, 1, -1, INT32_MIN, INT32_MAX, INT64_MIN}
    CHECK(t.same()); CHECK(t.jit.stats().native > 0);
}
TEST_CASE("CPU JIT 64-bit ops in 32-bit user mode still raise RI through the interpreter") { CHECK(t.same()); }
```

- [ ] **Step 2: FAIL. Step 3: Implementar. Step 4: PASS** (las cuatro variantes de suite N64).
- [ ] **Step 5: Repetir la validación de juegos de la Tarea 6, Step 5** (MK64 1900 en las cuatro combinaciones + `--state-check`).
- [ ] **Step 6: Commit** — `N64 JIT: native integer ALU`

---

### Task 8: Saltos nativos

**Files:**
- Modify: `src/core/n64/cpu/vr4300_jit.cpp` (o `vr4300_jit_branch.cpp`)
- Test: `tests/n64/test_cpu_jit.cpp`

`J JAL JR JALR BEQ BNE BLEZ BGTZ` y sus "likely", y `BLTZ BGEZ BLTZAL BGEZAL` (+likely). Destino calculado desde el PC **virtual**; enlace `pc + 8`. Con la ranura `Native` o `InPlace`, el salto y la ranura van en el mismo bloque y la salida final escribe `pc_ = destino`, `next_pc_ = destino + 4`, `branch_taken_ = false`. Likely no tomado: la ranura no se ejecuta (salida con una instrucción menos y sus ciclos). Si la ranura sale antes (`ExitBefore`), la salida deja `pc_ = ranura`, `next_pc_ = destino`, `branch_taken_ = true`, igual que el intérprete. Si el destino es el propio salto, se escribe `idle_candidate_ = destino`. `BC1T/BC1F` siguen `InPlace`.

- [ ] **Step 1: Test que falla**

```cpp
TEST_CASE("CPU JIT native branches match on every branch kind, taken and not") { CHECK(t.same()); }
TEST_CASE("CPU JIT native branch with a delay slot that exits before keeps the delay-slot state") {
    // slot = LW from MMIO (kseg1 0xA4400010): interpreter runs it with in_delay_slot
    CHECK(t.same());
}
TEST_CASE("CPU JIT native self-branch arms idle loop skipping") { CHECK(t.jit_cpu.idle_loop_cycles() == 2); }
```

- [ ] **Step 2–4:** FAIL → implementar → PASS. **Step 5:** validación de juegos como en la Tarea 7. **Step 6: Commit** — `N64 JIT: native branches`

---

### Task 9: Cargas y almacenamientos nativos en kseg0

**Files:**
- Modify: `src/core/n64/cpu/vr4300_jit.cpp` (o `vr4300_jit_memory.cpp`), `src/core/n64/rdp_worker.hpp` (vista), `src/core/n64/rdp.hpp`, `src/core/n64/bus.hpp`, `src/core/n64/cpu/cpu_bus.hpp`
- Test: `tests/n64/test_cpu_jit.cpp`, `tests/n64/test_rdp_worker.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] jit::PageBarrierView RdpWorker::barrier_view() const;` (punteros a `writers_`, `readers_`, `slot_sequence_`, `completed_seen_`, `failed_`); `Rdp::barrier_view()` → `std::optional<jit::PageBarrierView>` (vacío sin hilos); `virtual std::optional<jit::PageBarrierView> CpuBus::barrier_view() const` (vacío por defecto), que `Bus` implementa. `Vr4300Jit::execute()` lo lee en cada llamada.

`LB LBU LH LHU LW LWU LD SB SH SW SD` nativas: `vaddr = gpr[base] + sext(off)`; si `vaddr - 0xFFFFFFFF80000000 >= 0x20000000`, desalineada, o `paddr + bytes > ram_size` → salida antes (el intérprete hace kseg1, MMIO, TLB y la excepción). Si no: `barrier` (si hay vista), `load_ram`/`store_ram` y, en los almacenamientos, `note_ram_write` + `exit_if(changed)` *tras* la instrucción. `LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR`, `LL/SC`, COP1 siguen `Memory` (en el sitio).

- [ ] **Step 1: Tests que fallan**

```cpp
TEST_CASE("CPU JIT native kseg0 loads and stores match, including sign and zero extension") { CHECK(t.same()); }
TEST_CASE("CPU JIT native loads leave kseg1, MMIO, TLB and unaligned addresses to the interpreter") {
    CHECK(t.same()); CHECK(t.jit.stats().exits_before > 0);
}
TEST_CASE("CPU JIT native accesses call the RDP barrier exactly when the interpreter would wait") {
    // GuardBus-like bus exposing a PageBarrierView over local tables; count barrier_wait calls
    CHECK(waits == expected_waits);
}
TEST_CASE("RdpWorker barrier_view mirrors wait_memory's decision") { CHECK(view_says_wait == wait_memory_waited); }
```

- [ ] **Step 2–4:** FAIL → implementar → PASS (cuatro variantes N64).
- [ ] **Step 5:** validación de juegos completa de la Tarea 6, Step 5 (incluido n64-systemtest).
- [ ] **Step 6: Commit** — `N64 JIT: native kseg0 loads and stores with inline RDP barrier`

---

### Task 10: Fallos del RDP a través de bloques nativos

**Files:**
- Test: `tests/n64/test_cpu_jit.cpp`
- Modify: lo que el test descubra en `vr4300_jit.cpp`

- [ ] **Step 1: Test** (Review Focus 1)

```cpp
TEST_CASE("CPU JIT rethrows an RDP worker failure raised inside a block") {
    // bus whose barrier slow path / prepare_memory throws std::runtime_error("rdp failed")
    CHECK_THROWS_WITH_AS(jit.execute(10'000), "rdp failed", std::runtime_error);
    CHECK(jit_cpu_state_is_at_an_instruction_boundary);  // pc of the faulting access, nothing retired after it
}
```

- [ ] **Step 2:** ejecutar; si falla, corregir la captura en las ayudas. **Step 3:** PASS. **Step 4: Commit** — `N64 JIT: carry helper exceptions out of native code`

---

### Task 11: Medición y notas

**Files:**
- Create: `docs/performance/2026-11-18-n64-cpu-jit.md`
- Modify: `pendientes/recordatorios.md`

- [ ] **Step 1: Medir** en Release, 5 pares intercalados (alternando quién corre primero), carrera 1500–1900 con `--audio-hle --rdp-threaded --gfx-hle`, intérprete frente a `--cpu-jit`; y 3 pares sin `--gfx-hle`. Recoger las estadísticas del JIT (nativas / en el sitio / fuera de bloque / salidas antes / compilación / invalidaciones).
- [ ] **Step 2: Escribir el documento** con la tabla de pares, medianas y estadísticas, y aplicar el criterio aprobado: ganancia clara = mediana ≥ +10 % con ≥ 4/5 pares a favor. Sin ganancia clara: parar y replantear (no escribir el plan de la Fase 4 sin hablar con el usuario). Con ganancia: perfil nuevo y lista de lo que la Fase 4 atacaría primero.
- [ ] **Step 3: Actualizar `pendientes/recordatorios.md`** (pendiente marcado, bitácora con fecha), commit y `git push`. — `N64: measure the CPU JIT`
