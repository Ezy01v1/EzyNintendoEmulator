# Diseño: Nintendo 64, subproyecto 1 (CPU, memoria, arranque y video)

Fecha: 2026-11-06 · Rama: `n64/base`

## Intención y por qué

El roadmap del proyecto es GB/GBC → GBA → **N64** → Nintendo DS → GameCube → Wii. GB, GBC
y GBA están terminados. El usuario eligió para N64 el mismo enfoque que en las consolas
anteriores: **precisión primero**, validada con ROMs de prueba libres. Los juegos
comerciales llegan después.

N64 es demasiado grande para un solo spec. El usuario aprobó dividirlo en cuatro
subproyectos, cada uno con su spec, plan e implementación:

1. **Base** (este documento): CPU VR4300, memoria, interfaces, arranque, mandos y video del
   framebuffer.
2. **RSP**: núcleo escalar, unidad vectorial y DMA de SP.
3. **RDP**: rasterizador por software, propio y con precisión de píxel.
4. **Juegos**: audio completo, tipos de guardado, RTC y rendimiento.

## Objetivos

1. **CPU VR4300 por intérprete** (una instrucción por paso, con contador de ciclos), igual
   que el core de GBA:
   - 32 registros de 64 bits, HI/LO, PC, delay slots y branch-likely, LL/SC/LLD/SCD con
     `LLbit`, modos de direccionamiento de 32 y 64 bits (`Status.KX/SX/UX`) y modos
     kernel/supervisor/user.
   - Todas las instrucciones del conjunto MIPS III que implementa el VR4300, incluidas las
     de 64 bits, las trampas (`TEQ`, `TGEI`, …) y las de carga/almacenamiento desalineado
     (`LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR`).
   - **COP0**: `Index`, `Random`, `EntryLo0/1`, `Context`, `PageMask`, `Wired`, `BadVAddr`,
     `Count`, `EntryHi`, `Compare`, `Status`, `Cause`, `EPC`, `PRId`, `Config`, `LLAddr`,
     `WatchLo/Hi`, `XContext`, `ErrorEPC` y el resto con su comportamiento de lectura y
     escritura. TLB de 32 entradas (`TLBR`, `TLBWI`, `TLBWR`, `TLBP`) con traducción y las
     excepciones de TLB (refill, invalid, modified, en sus vectores de 32 y 64 bits), las
     de dirección, overflow, trampa, syscall, break, coprocessor unusable y reserved
     instruction, con `ERET`.
   - **COP1 (FPU)**: modos `FR=0` y `FR=1`, `FCR31` con modos de redondeo, flags, enables y
     causa, la excepción de punto flotante y "unimplemented operation" (por ejemplo con
     denormales), todas las conversiones y comparaciones.
   - La instrucción `CACHE` se acepta y no hace nada (ver "Decisiones").
2. **Bus y mapa de memoria**: RDRAM de 8 MB (como con Expansion Pak), registros de RDRAM y
   de RI, SP DMEM/IMEM con los registros de SP (el RSP siempre detenido en este
   subproyecto), registros de DP como stub, MI, VI, AI, PI, SI, ROM del cartucho y PIF
   ROM/RAM. Todo big-endian.
3. **Cartucho**: carga `.z64`, `.v64` y `.n64` (convierte a big-endian), lee la cabecera y
   detecta el CIC por checksum del IPL3 (6101, 6102, 6103, 6105, 6106 y 7101/7102).
4. **PIF**: PIF RAM, comandos joybus de mando (estado y lectura de botones y stick), el
   desafío del CIC al final del IPL3, y **arranque HLE**: hace lo que harían IPL1 e IPL2
   (copia 0x40–0xFFF de la ROM a SP DMEM, deja los registros de CPU y COP0 que dejaría el
   PIF según el CIC y la región, y salta a `0xA4000040`). Desde ahí se ejecuta el **IPL3
   real del cartucho**. Con una PIF ROM real (opcional) se arranca desde `0xBFC00000`.
5. **Interfaces**:
   - **MI**: modo, versión, interrupciones y máscara; entrega en `Cause.IP2`.
   - **PI**: DMA ROM↔RDRAM con duración calculada a partir de los registros de latencia,
     pulso, tamaño de página y release de cada dominio; estado e interrupción.
   - **SI**: DMA de PIF RAM con su duración e interrupción.
   - **VI**: todos los registros, contador de medias líneas, interrupción en `VI_V_INTR`,
     NTSC (60 Hz) y PAL (50 Hz) según la región del cartucho.
   - **AI**: registros, doble buffer de DMA con su duración a partir de `AI_DACRATE`,
     interrupción y muestras estéreo para el frontend.
6. **Scheduler de eventos** como el de GBA, en ciclos de CPU a 93,75 MHz: línea de VI, fin
   de DMA de PI, SI y AI, y Count/Compare (agendado en el ciclo exacto).
7. **Consola** (`n64::N64`): `create()`, `step()`, `run_frame()` (hasta la interrupción
   vertical de VI), save states con número de versión.
8. **Frontend**: `src/frontend/n64_system.cpp` implementa `System`. Lienzo fijo de 640×480:
   `render()` lee `VI_ORIGIN`, `VI_WIDTH`, el formato (16 bits 5551 o 32 bits) y las escalas
   de VI, y escala con nearest. Mando en el teclado. Ventana de debugger con registros de
   CPU/COP0/COP1 y desensamblado MIPS.
9. **Runner sin ventana** `tools/n64_runner`: `--screenshot <bmp> [frames]`,
   `--compare <bmp> [frames]` (sobre la imagen nativa de VI, no sobre el lienzo) y salida
   de texto ISViewer (`0x13FF0020`…) a la consola, para `n64-systemtest`.

## Decisiones

Aprobadas por el usuario durante el brainstorming:

- **Intérprete por instrucción + scheduler**, no un modelo ciclo a ciclo del pipeline de 5
  etapas ni un recompilador. Más adelante se le puede poner un caché de bloques sin cambiar
  la interfaz.
- **El contenido de los cachés no se simula.** `CACHE` no hace nada y los accesos con caché
  van directo a memoria, así que no se reproducen los fallos de coherencia entre caché y
  DMA.
- **Arranque HLE que ejecuta el IPL3 real**, con PIF ROM real opcional, como la BIOS de GBA.

Del asistente:

- **Costes de ciclos**: 1 ciclo por instrucción, más los extra del manual del VR4300 para
  `MULT/MULTU` (5), `DMULT/DMULTU` (8), `DIV/DIVU` (37), `DDIV/DDIVU` (69) y las operaciones
  de la FPU. Los accesos sin caché (kseg1, I/O, ROM) suman una penalización fija por tipo de
  destino, en constantes con nombre. Los accesos con caché no cuestan nada extra.
- **Count** avanza a la mitad del reloj de la CPU.
- **Acceso al bus**: tabla indexada por los bits altos de la dirección física, con camino
  rápido para RDRAM. Los accesos de 8, 16 y 64 bits se arman sobre palabras de 32.
- **Direcciones sin mapear**: leen 0 y las escrituras se ignoran (aviso en el log de debug),
  salvo los casos que mide `n64-systemtest`, que siguen lo que espera.
- **Instrucciones reservadas**: excepción Reserved Instruction, como el hardware.
- **RSP y RDP**: si un programa pone en marcha el RSP o manda comandos al RDP, se registra
  una vez y se ignora. El RSP sigue detenido.

## No objetivos

- RSP y RDP (subproyectos 2 y 3). Por eso ningún juego comercial pasará del arranque.
- Contenido de los cachés del VR4300.
- Tipos de guardado (EEPROM, SRAM, FlashRAM, Controller Pak), RTC, Rumble Pak, 64DD.
- Filtros y antialiasing de VI.
- Velocidad real. Se mide y se reporta, pero no se exige.

## Anti-objetivos (fallar aunque "funcione")

- Saltarse el IPL3 copiando el juego directo a RDRAM: el IPL3 real tiene que ejecutarse.
- Ajustar una constante para que pase una ROM de prueba sin entender qué modela; cada
  calibración se explica en un comentario con la ROM que la fija.
- Comparar capturas sobre el lienzo escalado de 640×480 en vez de la imagen nativa de VI.

## Verificación

- **Tests unitarios** en `tests/n64` (doctest): semántica de instrucciones (incluidos
  overflow y casos de 64 bits), delay slots y excepciones dentro de ellos, TLB, redondeo y
  excepciones de la FPU, mapa del bus, DMA de PI y SI, temporización de VI, conversión de
  orden de bytes, detección del CIC y estado tras el arranque HLE.
- **ROMs de prueba** en ctest (configuración Release, solo si están en `roms/n64/`):
  - PeterLemon/N64 `CPUTest` (CPU, CP0, CP1, Exceptions, LOADSTORE, DMAAlignment-PI-cart)
    con `n64_runner --compare` contra sus PNG convertidos a BMP de 24 bits.
  - `n64-systemtest` (lemmy-64), compilado localmente con Rust nightly, leyendo su reporte
    por ISViewer.
- **Terminado cuando**:
  - pasan todas las pruebas de PeterLemon listadas arriba;
  - en `n64-systemtest` pasan todos los grupos que no dependen del RSP ni del RDP; los que
    fallan quedan listados en `roms/README.md` con su motivo;
  - pasan los tests unitarios y el resto de ctest (GB, GBA, CHIP-8).
- **Rendimiento**: tiempo de `n64-systemtest` y de una prueba de PeterLemon, reportados en
  `roms/README.md`.

## Restricciones

- Compilar solo con los presets de CMake en `build/default`.
- Código, comentarios y commits en inglés; el estilo de `src/core/gba` (C++20,
  `[[nodiscard]]`, `std::span`, comentarios que explican el hardware).
- Las ROMs y la PIF ROM no se suben al repo (`roms/` está ignorado por git).
- Un commit por pieza de hardware, en la rama `n64/base`, con PR a `main`.

## Lo que queda al builder

- El orden de los pasos y cómo dividir los commits.
- La forma del formato del save state y de las opciones de la CLI del runner.
- Las teclas del mando en el frontend.
- Los valores exactos de las penalizaciones de acceso sin caché, mientras se expliquen.
