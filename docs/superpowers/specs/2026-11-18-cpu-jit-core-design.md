# Núcleo de JIT compartido, validado en la CPU del N64 (2026-11-18)

## Para qué

El usuario quiere que ezyemu corra Game Boy, N64, GameCube y Wii con fluidez. Preguntado si
un recompilador debía servir a "MK64 a velocidad completa pronto" o a "una base de JIT para
GameCube/Wii", respondió **ambas**. GameCube y Wii no son jugables sin un JIT de CPU, y en N64
la CPU emulada es hoy el límite: con `--audio-hle --rdp-threaded --gfx-hle`, MK64 en carrera
(campos 1500–1900) va a **53,8 campos/s** (`docs/performance/2026-11-18-n64-dma-barriers.md`)
y el hilo principal es el límite; la CPU emulada se estimó en ~65 % de sus muestras
(`pendientes/recordatorios.md`), cifra que no está documentada con un perfil. El plan empieza
por un `--profile` nuevo de la carrera que confirme esa parte y diga qué instrucciones y
segmentos de memoria dominan.

Este proyecto construye un JIT **general** (entrada de un ISA → operaciones neutras →
optimizaciones → x86-64) y lo prueba primero con una entrada MIPS para la CPU del N64, donde
existe un intérprete correcto contra el que comparar estado por estado. GameCube/Wii añadirá
después una entrada PowerPC sobre el mismo núcleo.

## Metas

1. **Mismo resultado exacto que el intérprete.** Tras N campos, el estado completo
   (`--dump-state`) con JIT es byte a byte el del intérprete, con RDP síncrono y con hilos,
   con y sin HLE gráfico. Las imágenes y la continuación de estados entre modos coinciden.
2. **MK64 a 60 campos/s o más** en la carrera (1500–1900, `--audio-hle --rdp-threaded
   --gfx-hle`), medido en pares intercalados contra el intérprete como en las mediciones
   anteriores. 60 campos/s es la velocidad real NTSC.
3. **Núcleo reutilizable:** búfer de código, operaciones neutras (IR), asignación de
   registros, backend x86-64, caché de bloques, invalidación por páginas, presupuesto de
   ciclos y salidas al intérprete no saben nada de MIPS. La entrada MIPS es una capa fina.

## No metas (fuera de este proyecto)

- La entrada PowerPC, GameCube/Wii en sí, ARM64, dibujar en la GPU, el JIT del RSP.
- Velocidad en juegos que usan mucho la TLB (GoldenEye, Perfect Dark): deben funcionar
  igual que con el intérprete, aunque sin ganar velocidad en el código mapeado (ver Fase 5).
  Excepción: si el perfil muestra que el código o los datos calientes de MK64 pasan por la
  TLB, la Fase 5 entra en el proyecto, porque sin ella no se llega a la meta 2.
- Ganar velocidad con el RSP en LLE: con el RSP corriendo microcódigo hay un evento
  `RspStep` cada 1–2 ciclos y los bloques casi nunca caben en el presupuesto. Con HLE de
  audio y gráficos el RSP casi no corre, que es la configuración de la meta 2.

## Anti-metas (fracaso aunque "funcione")

- Cualquier divergencia de estado con el intérprete, aunque el juego "se vea bien".
- Un JIT que no sea más rápido que el intérprete en la meta 2. Ya pasó con el JIT del RSP
  (`2026-11-10-n64-rsp-jit-design.md`: la compilación y la entrada a bloques dominaron).
  Si tras la Fase 4 no hay ganancia clara en pares, se para y se replantea antes de seguir.
- Activarlo por defecto antes de que el usuario lo pruebe jugando, como con el HLE gráfico.
- Un IR tan atado a MIPS (ranuras de retardo, HI/LO, registro cero) que PowerPC obligue a
  rehacerlo.

## Restricciones

- C++20, MSVC Build Tools 2022, Windows x64; solo los presets de CMake.
- AsmJit, ya fijado por SHA en `src/core/n64/CMakeLists.txt` para el JIT del RSP. Hoy solo
  se descarga dentro de `if(EZY_N64_RSP_JIT AND EZY_N64_HOST_X64)` y se enlaza PRIVATE a
  `ezy_n64`; la descarga (y la detección de host x64) sube a un nivel común para que
  `ezy_jit` lo tenga aunque el JIT del RSP esté apagado. El JIT de CPU va tras su propia
  opción `EZY_N64_CPU_JIT` (ON por defecto en hosts x64, como `EZY_N64_RSP_JIT`); con la
  opción OFF todo compila y las suites pasan sin AsmJit.
- Activación en tiempo de ejecución: `--cpu-jit` en `ezy_n64_runner` y en `ezyemu`,
  apagado por defecto. El intérprete sigue siendo la referencia y el respaldo.
- Formato de estados sin cambios: el JIT no guarda nada; cargar un estado vacía su caché.
- Responder en español; código, commits y PRs en inglés.

## Cómo es hoy el bucle de la CPU

`N64::step()` (src/core/n64/n64.cpp): salto de bucles ociosos, luego `cpu_.step()`
(una instrucción), `scheduler_.advance(ciclos)` y `run_due_events(false)`. `Vr4300::step()`
comprueba la interrupción al empezar, busca la instrucción, ejecuta, pone `gpr_[0] = 0` y
llama `cop0_.retire(ciclos)` (Count/Compare/IP7 y Random). Los ciclos son 1 más los extra de
multiplicaciones, divisiones y FPU (constantes en `vr4300_ops.cpp` y `cop1.cpp`) y la
penalización de cada acceso sin caché. Las lecturas y escrituras de RDRAM van directas a
`direct_ram()` tras `guard_memory()` (la barrera del RDP con hilos); el resto pasa por los
métodos virtuales de `CpuBus` y puede programar eventos o cambiar la línea de interrupción.

## Enfoques considerados

| | Qué es | A favor | En contra |
|---|---|---|---|
| A. MIPS → x86 directo | Como el JIT del RSP: cada instrucción emite x86 | El más rápido de construir | Nada pasa a PowerPC salvo la caché y el despachador |
| **B. IR pequeño + backend x86 (elegido)** | MIPS → lista lineal de operaciones neutras por bloque → pasadas sencillas → x86-64 con AsmJit | El backend, la asignación de registros y la infraestructura sirven para PowerPC; las optimizaciones se escriben una vez | ~30 % más trabajo inicial que A |
| C. Backend externo (LLVM u otro) | Generar IR de LLVM | Optimizaciones gratis | Compila demasiado lento para bloques de un emulador; dependencia enorme |

Se elige **B** porque el usuario quiere las dos cosas: A acelera el N64 pero deja casi todo
por rehacer para GameCube, y C repetiría el problema del JIT del RSP (coste de compilar).
Decisión del agente; el usuario delegó el cómo.

## El diseño

### Piezas y dónde viven

- `src/core/jit/` (biblioteca `ezy_jit`, espacio `ezy::jit`), sin dependencias de N64:
  - **IR**: valores de 32 y 64 bits, enteros y flotantes (f32, f64 y un par de f32 para los
    "paired singles" de PowerPC, aunque MIPS no lo use todavía); operaciones aritméticas,
    lógicas, desplazamientos, comparaciones que dan un booleano, selección, extensión de
    signo, y operaciones con acarreo/desbordamiento como valores explícitos (no banderas
    implícitas), que PowerPC (XER.CA/OV, CR) necesitará. Lectura/escritura de "registros del
    invitado" como desplazamientos en una estructura de estado que define cada entrada, no
    como registros MIPS. Carga/almacenamiento de memoria con tamaño y orden de bytes.
    Llamadas a funciones de ayuda. Salidas: a una dirección fija, a una dirección calculada,
    y "salida lateral" al intérprete en una instrucción dada.
  - **Pasadas**: propagación de constantes, eliminación de escrituras de registros del
    invitado que se sobrescriben antes de leerse, y asignación de registros del host por
    bloque (barrido lineal). Nada más en este proyecto.
  - **Backend x86-64** con AsmJit, búfer de código con presupuesto y vaciado completo al
    agotarse (la lección del RSP: reset duro del asignador fuera de código nativo).
  - **Caché de bloques y despachador**: tabla de dos niveles por dirección física (páginas
    de 4 KiB reservadas a demanda), enlace directo entre bloques cuyo destino es fijo, y
    desenlace al invalidar.
  - **Seguimiento de páginas con código**: un byte por página física; quien escribe en una
    página con código invalida sus bloques.
- `src/core/n64/cpu/vr4300_jit.{hpp,cpp}`: la entrada MIPS (decodificar → IR), las ayudas que
  llaman al intérprete, y el enganche con `Vr4300`/`N64`. `Vr4300` expone lo que haga falta
  (estructura de estado con offsets estables, ejecutar una instrucción "en el sitio").

### Qué corre el JIT y qué el intérprete

Un bloque empieza en una instrucción y termina tras un salto y su ranura de retardo, antes
de una instrucción que el bloque no puede ejecutar, o al llegar a un máximo de instrucciones
(el constructor elige el máximo). Cada instrucción del bloque es de una de tres clases:

1. **Nativa**: la entrada MIPS la traduce a IR. Para empezar: ALU entera de 32 y 64 bits,
   desplazamientos, `LUI`, saltos y saltos "likely", `MULT/DIV` y familia (con sus ciclos
   extra fijos), y cargas/almacenamientos a RDRAM por kseg0 (ver Memoria).
2. **Interpretada en el sitio**: una llamada desde el bloque a la rutina del intérprete
   para esa instrucción, con el estado volcado antes. Vale para instrucciones sin efectos
   en el bus: FPU (COP1), `MFHI/MTLO` si no se traducen aún, etc. Si la rutina levanta una
   excepción, el bloque sale en ese punto con el estado exacto. Sus ciclos extra se suman
   al contador del bloque. Esto deja crecer la entrada MIPS poco a poco sin cortar bloques,
   y la Fase 4 vuelve nativas las de FPU más frecuentes.
3. **Fuera del bloque**: el bloque termina *antes* de ella y la ejecuta `Vr4300::step()`
   como hoy, seguido de los eventos. Son las instrucciones que pueden programar eventos,
   cambiar interrupciones o el modo: COP0 (`MTC0`, `MFC0` de Count/Random, `ERET`, TLB),
   `SYSCALL`, `BREAK`, `CACHE`, y cualquier acceso que no sea RDRAM por kseg0.

Una instrucción que puede levantar una excepción nativa (desbordamiento de `ADD/ADDI/DADD`,
dirección desalineada, `TEQ` y trampas) se comprueba en línea; si fallaría, el bloque sale
**antes** de ella y el intérprete la ejecuta y levanta la excepción. Así toda excepción la
levanta siempre el mismo código que hoy.

### Tiempo exacto: el presupuesto

Todo lo que puede cambiar el resultado del intérprete entre instrucciones viene de un
evento del scheduler, de que Count alcance Compare, o de la propia instrucción (bus, COP0).
Las instrucciones de la tercera clase ya quedan fuera de los bloques. Para las otras dos:

- Al entrar al JIT, el despachador calcula `limit = min(evento más próximo de todos los
  tipos, now + cycles_until_compare())`, con la misma aritmética estricta que el salto de
  bucles ociosos en `N64::step()`.
- Un bloque corre entero solo si `now + ciclos_del_bloque < limit`. Si no cabe, el
  intérprete ejecuta instrucciones sueltas hasta que llegue el evento, como hoy. Sus ciclos
  se conocen al compilarlo; las instrucciones interpretadas en el sitio que añaden ciclos
  variables comprueban el presupuesto tras la llamada y salen si se pasó.
- Varios bloques seguidos corren dentro de la misma entrada (enlazados en código nativo)
  mientras quede presupuesto: entre ellos no cae ningún evento ni cambia la interrupción.
- Al salir: `scheduler_.advance(ciclos)`, `cop0_.retire_many(ciclos, instrucciones)` (Count
  y Random a la vez) y `run_due_events(false)`.
- No se entra al JIT si `interrupt_pending()` o si la CPU está en una ranura de retardo
  (`branch_taken_`): el intérprete da ese paso. El salto de bucles ociosos sigue antes del
  JIT tal cual.

### Memoria

- Ruta rápida en línea: modo kernel, dirección en kseg0 y física dentro de RDRAM → carga o
  almacenamiento directo en `direct_ram()` con intercambio de bytes (RDRAM está en
  big-endian). Cualquier otra dirección sale del bloque antes de la instrucción.
  La entrada MIPS solo emite la ruta rápida; el núcleo la recibe como "base + límite +
  salida lenta", lo que PowerPC usará igual con su propia traducción.
- **Barrera del RDP con hilos**: con el RDP en hilos, cada acceso hace hoy
  `guard_memory()`. El JIT emite en línea la parte barata de `RdpWorker::wait_memory()`
  (página → ranura → secuencia, comparada con `completed_seen_`) y solo llama a la función
  si la página tiene un paquete más nuevo. `RdpWorker` expone una vista de solo lectura de
  esas tablas para el JIT; sigue siendo el único que las escribe.
- **Invalidación**: el almacenamiento en línea mira el byte de su página en la tabla de
  páginas con código; si está marcado, llama a la ayuda que invalida los bloques de esa
  página y, si es el bloque que corre, sale tras el almacenamiento. Los demás escritores
  de RDRAM avisan con su rango:
  - DMA de PI, SI y SP y audio HLE: desde el `Bus`, en los mismos puntos donde ya ponen las
    barreras del RDP (`prepare_range`, `prepare_pi_dma`, las filas de DMA de SP).
  - El RDP (síncrono o en hilos) y el HLE gráfico **no** pasan por esos puntos: escriben
    dentro de `rdp_.step(...)` / `submit_words` y de `f3dex_.run_task(...)`, y
    `Rdp::prepare_memory` no hace nada en modo síncrono. Necesitan su propio aviso: el RDP,
    por ejemplo, con los rangos de color/profundidad que ya calcula para las reservas de
    `RdpWorker` (`RdpAccess`), avisados en el hilo principal al enviar el paquete.
  El constructor hace la lista completa y añade un modo de comprobación (ver Validación)
  que detecta un escritor olvidado.
- Granularidad: páginas de 4 KiB para empezar. Si el contador de invalidaciones muestra
  que datos y código comparten páginas y se recompila de más, se afina a líneas de 32 bytes
  dentro de la página.

### Clave de los bloques

El código compilado depende del modo (kernel/supervisor/usuario), de `CU1`, `FR` y
`KX/SX/UX` (operaciones de 64 bits permitidas). Solo cambian con instrucciones de la
tercera clase o excepciones, así que el despachador calcula la clave al entrar y busca
bloques con esa clave; el enlace directo solo une bloques de la misma clave.

También depende de la dirección **virtual**: los destinos de `J/JAL` y el valor de enlace
salen del PC virtual (`vr4300_ops.cpp`), y buscar la instrucción por kseg1 suma ciclos sin
caché (`Vr4300::load`). En este proyecto solo se compila código que se busca por kseg0 en
modo kernel, donde virtual y física van uno a uno (`física = virtual & 0x1FFFFFFF`); el
código en kseg1 o mapeado por TLB lo sigue ejecutando el intérprete (la Fase 5 lo amplía).
Los bloques se buscan por PC virtual + clave de modo; la dirección física sirve para la
invalidación.

Los saltos nativos deben dejar el mismo estado auxiliar que el intérprete, en particular
`idle_candidate_` (`vr4300_ops.cpp`), para que el salto de bucles ociosos siga activándose.

### Estadísticas y opciones

Contadores como los del RSP: bloques compilados, instrucciones nativas / interpretadas en
el sitio / fuera de bloque, tiempo de compilación, invalidaciones, vaciados del búfer.
`--benchmark` los imprime con `--cpu-jit`.

## Validación

- **Diferencial por instrucción** (pruebas con un `CpuBus` plano, como las de la CPU hoy):
  secuencias generadas de instrucciones con registros aleatorios corren en el intérprete y
  en el JIT desde el mismo estado; se compara el estado completo de la CPU (GPR, HI/LO, PC,
  Count, Random, Cause, COP1, FCSR) y la RAM. Incluye bordes: desbordamientos, ranuras de
  retardo, saltos likely no tomados, excepciones a mitad de bloque, bloques que no caben en
  el presupuesto, Compare alcanzado justo al final.
- **Invalidación**: código automodificado en el mismo bloque y en otro, DMA de PI sobre
  código ya compilado, y un modo `--cpu-jit-check` (solo pruebas y diagnóstico) que antes de
  entrar a cada bloque compara los bytes de la RAM con los que se compilaron y aborta con
  la dirección si difieren.
- **Juego completo**: MK64 `--dump-state` a 1900 campos idéntico entre intérprete y JIT,
  con RDP síncrono y con hilos, con y sin HLE gráfico; `--state-check 1700` cruzando modos
  en ambas direcciones; imágenes iguales a 120/300/600 campos; Spider-Man igual.
- n64-systemtest con `--cpu-jit`: mismo resultado que el intérprete (2984/3721).
- Las ocho suites, con `EZY_N64_CPU_JIT` ON y OFF.
- **Velocidad**: pares intercalados contra el intérprete (al menos 5), medianas, como en
  `docs/performance/`. Resultados en un documento nuevo de `docs/performance/`.

## Fases

1. **Núcleo**: IR, backend x86-64, búfer de código, despachador y caché; pruebas del núcleo
   solo con IR escrito a mano (sin MIPS).
2. **Entrada MIPS mínima**: ALU entera, saltos, kseg0 RDRAM, todo lo demás interpretado en
   el sitio o fuera de bloque; presupuesto exacto; pruebas diferenciales; MK64 idéntico.
3. **Invalidación completa** de todos los escritores de RDRAM y el modo de comprobación.
4. **Velocidad**: enlace de bloques, asignación de registros, barrera del RDP en línea, FPU
   nativa para las operaciones frecuentes de MK64 (perfil primero). Medición en pares.
   Criterio de salida (decisión del agente, a confirmar por el usuario):
   - "Ganancia clara" = mediana al menos **+10 %** sobre el intérprete en la carrera con
     HLE, con al menos 4 de 5 pares a favor, en la misma máquina de la línea base de 53,8.
   - Sin ganancia clara: parar y replantear (anti-meta), sin añadir más optimizaciones.
   - Con ganancia clara pero por debajo de 60 campos/s: se cierra el proyecto con lo
     ganado, se documenta el perfil que queda y lo siguiente (más FPU nativa, registros
     entre bloques, RSP perezoso) se propone como una ronda nueva.
5. **Opcional, si un juego de prueba lo pide**: tabla de páginas virtuales → host para el
   código y los datos mapeados por TLB, vaciada en `TLBW*` y cambios de ASID. Es la misma
   pieza que necesitará la traducción de direcciones de PowerPC.

Al final: el usuario prueba MK64 en `ezyemu --gfx-hle --cpu-jit` con su mando; si va bien,
se decide activarlo por defecto (no antes).

## Lo que queda al constructor

- El formato concreto del IR (estructura de las operaciones, cómo se numeran los valores),
  el algoritmo exacto de asignación de registros y la convención de llamadas a ayudas.
- El máximo de instrucciones por bloque y el tamaño del búfer de código.
- Qué instrucciones pasan de "interpretada en el sitio" a nativa en la Fase 4, guiado por
  el perfil de MK64.
- Si `LL/SC` y las cargas/almacenamientos de COP1 son nativos o fuera de bloque.
- Relajar la condición estricta del presupuesto (`<` frente a `<=`), solo con una prueba
  que demuestre que el resultado no cambia.
