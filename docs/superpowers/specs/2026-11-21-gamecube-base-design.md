# GameCube: hoja de ruta y subproyecto 1, base y arranque de MKDD (2026-11-21)

## Para qué

El usuario quiere jugar GameCube y Wii con fluidez en ezyemu (meta de fondo, ver
`pendientes/recordatorios.md`). El 2026-11-21 eligió empezar GameCube, saltándose Nintendo DS
en la hoja de ruta. Primer objetivo, en sus palabras: **que Mario Kart: Double Dash!! llegue a
la pantalla de título**. Está descargando el volcado de MKDD en `E:\Rom nintendo` (junto a las
demás ROMs; nunca entra al repo).

El usuario delegó el diseño ("toma la mejor decisión para el proyecto") y no estaba presente:
todo lo que sigue es **decisión del agente**, para que el usuario lo revise al volver. Esa
delegación cuenta como aprobación del diseño para avanzar.

## Hoja de ruta de GameCube (subproyectos)

La pantalla de título de MKDD necesita CPU, periféricos, el lector de disco y la GPU dibujando,
y probablemente el DSP respondiendo. Es demasiado para un solo diseño, así que se parte en
subproyectos, cada uno con su diseño y plan:

1. **Base y arranque (este documento).** La CPU Gekko interpretada, la memoria, los periféricos
   (PI, MI, VI, DI, SI, EXI, AI, interfaz DSP), el disco ISO/GCM, el arranque HLE, el FIFO de
   comandos gráficos (recibidos y contados, todavía sin dibujar), un runner sin ventana y la
   consola en ezyemu. Hito: MKDD arranca desde el disco, termina su inicialización y entra en su
   bucle principal, enviando listas de comandos gráficos cada campo, sin instrucciones ni
   registros sin implementar.
2. **GX en la GPU del host.** Procesador de comandos, transformación (XF), rasterizado, el
   combinador TEV, texturas y la copia del EFB al XFB, dibujado en la tarjeta de video. Hito:
   **la pantalla de título de MKDD se ve**. Cambia el frontend, que hoy solo recibe píxeles de la
   CPU (`System::render`).
3. **Audio.** El DSP (HLE del microcódigo que use MKDD o LLE) y el streaming del AI. Hito: música
   de la pantalla de título.
4. **JIT de Gekko** sobre `ezy_jit`: entrada PowerPC, FPU y paired singles nativos (el asignador
   gana clases de registros XMM), la regla de reanudación revisada para salidas MMIO
   deterministas (nota en recordatorios). Hito: MKDD a velocidad completa.
5. **Después:** tarjeta de memoria, más juegos, y Wii (Broadway/Hollywood, IOS en HLE).

## Subproyecto 1: metas

1. **Arranque:** con la imagen de disco de MKDD (ISO/GCM), la máquina carga el ejecutable
   principal (DOL), arranca en su punto de entrada y llega al bucle principal del juego.
   Criterio medible en el runner: durante al menos 600 campos seguidos (10 s emulados), cada
   campo el juego escribe datos al FIFO gráfico, el VI da sus interrupciones, y la ejecución no
   se detiene por una instrucción, un registro de E/S o un comando de disco sin implementar.
2. **CPU correcta:** cada instrucción de Gekko que se implemente tiene pruebas unitarias con los
   valores del manual (incluidos CR0, XER[CA/OV/SO], redondeo de precisión simple, paired
   singles y cuantización GQR).
3. **Imagen del XFB:** el runner guarda el framebuffer externo (YUV 4:2:2 en RAM, lo que el VI
   envía a la TV) como BMP. Hasta el subproyecto 2 solo mostrará lo que el juego escriba ahí sin
   GX (por ejemplo, el color de borrado de las copias del EFB).
4. **ezyemu** abre `.iso`/`.gcm` como GameCube, con el mando de Xbox y el teclado mapeados al
   mando de GameCube, y muestra el XFB.
5. Las ocho suites actuales siguen pasando. Nada de N64, GB ni GBA cambia.

## No metas (de este subproyecto)

- Dibujar GX (subproyecto 2). Los comandos se reciben, se analizan lo justo para saber su
  tamaño y para responder a lo que el juego espera (tokens, "draw done", copias del EFB con
  borrado), y se cuentan.
- Audio real (subproyecto 3): el DSP solo responde lo necesario para que el juego no se quede
  esperando.
- JIT (subproyecto 4), velocidad completa, tarjeta de memoria (las ranuras aparecen vacías),
  tablas de páginas de la MMU (solo BAT), Wii.
- Arranque con la BIOS (IPL) del usuario: el arranque es HLE.
- Formatos comprimidos de disco (RVZ, NKit, CISO). Si el volcado que llegue usa uno, se decide
  al verlo (regla de la sección "Disco").

## Anti-metas

- Una base atada a MKDD con trucos por juego (parches de memoria, saltos por dirección): todo
  lo que se haga debe ser comportamiento del hardware o del arranque HLE documentado.
- Un intérprete sin pruebas: será la referencia del JIT, como en N64.
- Romper otras consolas o cambiar la interfaz `System` más de lo necesario.

## Restricciones

- C++20, MSVC Build Tools 2022, Windows x64, solo presets de CMake, `/W4` limpio.
- Sin dependencias nuevas en este subproyecto.
- El disco y las ROMs nunca entran al repo. Sin BIOS ni microcódigos con copyright en el repo.
- Español en documentos y notas; código, commits y PRs en inglés.
- Rama `gc/base` desde `main`.

## Enfoques considerados

| | Qué es | A favor | En contra |
|---|---|---|---|
| **A. Intérprete + arranque HLE primero (elegido)** | Gekko interpretado con pruebas, el disco cargado directamente por el emulador, y periféricos mínimos pero reales | Es la referencia exacta que necesitará el JIT (como en N64); se puede depurar instrucción a instrucción; no necesita la BIOS | Lento (MKDD no irá a velocidad completa hasta el subproyecto 4) |
| B. JIT desde el principio | Entrada PowerPC directa sobre `ezy_jit` | Velocidad antes | Sin una referencia, cada error del JIT y de la emulación se mezclan; la FPU y los paired singles aún no existen en el núcleo |
| C. Arranque con la BIOS (IPL) | Ejecutar el IPL real del usuario | El arranque más fiel | Requiere un volcado de BIOS que el usuario no tiene, más hardware (DSP, tarjeta, fuentes) antes de ver nada |

Se elige **A** por la misma razón que funcionó en N64: primero un intérprete correcto y probado,
después el JIT comparado contra él.

## El diseño

### Dónde vive

- `src/core/gc/` (biblioteca `ezy_gc`, espacio `ezy::gc`), con la misma forma que `src/core/n64`:
  `gekko/` (CPU), `memory`, `scheduler`, un archivo por periférico, `disc`, `dol`, `hle_boot`,
  `gx_fifo`, `gamecube.{hpp,cpp}` (la máquina). Pruebas en `tests/gc/`, runner en
  `tools/gc_runner/` (`ezy_gc_runner`), consola en `src/frontend/gamecube_system.{hpp,cpp}`.
- Estados guardados: no en este subproyecto.

### CPU Gekko (intérprete)

- Registros: 32 GPR, 32 FPR con dos mitades (ps0/ps1), CR, LR, CTR, XER, MSR, FPSCR, SRR0/1,
  DAR, DSISR, DEC, TB, SPRG0–3, HID0/1/2/4, GQR0–7, WPAR, DMAU/DMAL, L2CR, los 8 BAT de datos y
  8 de instrucciones, PVR (0x00083214).
- Traducción solo por BAT (MSR[IR]/[DR]). Con la traducción apagada, la dirección es física. Una
  dirección sin BAT que la cubra da DSI/ISI; las tablas de páginas no se implementan (no meta).
- Instrucciones: todo el conjunto entero de 32 bits (aritmética con CA/OV/Rc, lógicas,
  rotaciones `rlw*`, comparaciones, saltos con BO/BI y LR/CTR, cargas y almacenamientos con y sin
  actualización, indexados, con inversión de bytes, `lmw/stmw`, `lswi/stswi` si aparecen,
  `mtcrf/mfcr/mcrf/mcrxr`, operaciones de CR), `mfspr/mtspr`, `mftb`, `mfmsr/mtmsr`, `sc`,
  `rfi`, `sync/isync/eieio`, cachés (`dcbf/dcbi/dcbst/dcbt/dcbtst/dcbz/dcbz_l/icbi` como
  operaciones sin efecto o que escriben ceros, según corresponda), FPU de doble precisión con
  sus formas de precisión simple (que redondean a single), conversiones (`fctiw[z]`, `frsp`),
  `mtfsf*/mffs`, comparaciones de FPU, y **paired singles** (`ps_*`, `psq_l[u][x]`,
  `psq_st[u][x]` con cuantización según GQR, `ps_merge*`, `ps_sum*`, `ps_muls*`, `ps_madds*`).
  Modo de redondeo del FPSCR respetado; excepciones de FPU no habilitadas (los juegos no las usan).
- Excepciones: reset, DSI, ISI, externa (las interrupciones de PI), alineación si hace falta,
  programa (instrucción ilegal, trap), FP no disponible, decrementador, llamada al sistema.
  Vectores en `0x00000100…0x00000F00` (con MSR[IP] apagado). SRR0/SRR1 como dice el manual.
- Una instrucción sin implementar detiene la máquina con un error que da PC y palabra (en el
  runner y en las pruebas), nunca un "no hace nada" silencioso.
- Tiempo: 486 MHz de CPU; para este subproyecto cada instrucción cuenta como 1 ciclo (la
  precisión de ciclos no hace falta para arrancar). El time base y el decrementador avanzan a
  la cuarta parte del bus (162 MHz / 4 = 40,5 MHz), es decir, 1 tick cada 12 ciclos de CPU.

### Memoria y bus

- MEM1: 24 MiB en la física `0x00000000`. ARAM: 16 MiB, solo accesible por DMA del DSP.
- Registros de hardware en la física `0x0C000000` (virtual `0xCC000000` con los BAT del arranque):
  CP `0x0C000000`, PE `0x0C001000`, VI `0x0C002000`, PI `0x0C003000`, MI `0x0C004000`, DSP
  `0x0C005000`, DI `0x0C006000`, SI `0x0C006400`, EXI `0x0C006800`, AI `0x0C006C00`, el puerto
  de escritura del FIFO gráfico ("write-gather pipe") en `0x0C008000`.
- Caché bloqueada (locked cache, 16 KiB en `0xE0000000` cuando HID2 la activa) y el DMA de
  caché (DMAU/DMAL) entre ella y MEM1.
- Big-endian en todos los accesos. Un acceso a una dirección física sin dispositivo detiene la
  máquina con un error (como las instrucciones).

### Disco

- Lector de imágenes ISO/GCM (1 459 978 240 bytes, sin compresión). Cabecera: identificador de
  juego (MKDD USA = `GM4E01`), offset del DOL (`0x420`), del FST (`0x424`, tamaño `0x428`).
- **Regla para el volcado que llegue:** si es `.iso`/`.gcm`, se usa tal cual. Si viene en `.zip`
  o `.7z`, se extrae (como se hizo con MK64 y Spider-Man). Si es RVZ, NKit o CISO, el agente lo
  anota y el subproyecto 1 se prueba con lo que haya (por ejemplo, una imagen sintética en las
  pruebas), y la conversión o un lector de RVZ se decide entonces (RVZ necesita zstd/LZMA:
  dependencia nueva).
- Interfaz DI: comandos Inquiry (`0x12`), Read (`0xA8`, con DMA a MEM1 y su interrupción de fin),
  Seek (`0xAB`), Request Error (`0xE0`), los de audio en streaming (`0xE1`, `0xE2`, `0xE4`) como
  respuestas "apagado", estado de la tapa cerrada. La lectura tarda un tiempo fijo razonable
  (programado en el scheduler), no cero, para que los juegos que esperan la interrupción la vean
  llegar después.

### Arranque HLE

En vez del IPL, el emulador deja la máquina como la deja la BIOS al saltar al juego:

- Copia la cabecera del disco a `0x80000000` (los primeros 0x20 bytes: identificador, versión…).
- Rellena las variables globales bajas que el SDK lee al iniciar: magia `0x0D15EA5E` en
  `0x80000020`, versión, tamaño de memoria `0x01800000` en `0x80000028`, tipo de consola
  (retail) en `0x8000002C`, arena baja y alta en `0x80000030/34`, FST en `0x80000038/3C`, modo de
  video en `0x800000CC`, reloj del bus `162000000` en `0x800000F8`, de CPU `486000000` en
  `0x800000FC`. La lista exacta (y lo que el apploader deja que un DOL cargado directamente no
  deja) se toma de la documentación pública (YAGCD, el código abierto de Dolphin) y se cita en el
  código.
- Carga el DOL (7 secciones de texto, 11 de datos, BSS a cero) y el FST al final de la memoria.
- Registros: BAT como el IPL (IBAT0/DBAT0: `0x80000000` → física 0, 256 MiB, con caché; DBAT1:
  `0xC0000000` → física 0, sin caché), MSR con traducción y FPU activadas, `r1` con una pila
  válida, PC = punto de entrada del DOL.
- Si MKDD necesita algo que solo deja el apploader, se añade al arranque HLE (o se ejecuta el
  apploader del disco con el CPU emulado, como hace Dolphin); el agente decide con lo observado.

### Periféricos (lo mínimo real)

- **PI:** causa y máscara de interrupciones (y la línea externa del Gekko), código de reset, el
  FIFO del CP en RAM (base, final, puntero de escritura que avanza con el write-gather pipe).
- **MI:** registros de protección de memoria (aceptados, sin efecto) e interrupción.
- **VI:** registros, contador de líneas (NTSC 525 líneas, 60 Hz entrelazado, también 240p si el
  juego lo pide), las 4 interrupciones de línea (`VI_DI0..3`), dirección del XFB superior/inferior.
  Imagen: el XFB en YUV 4:2:2 convertido a ARGB (BT.601), entrelazado según el campo.
- **SI:** sondeo del mando 1 (los demás desconectados): identificación `0x0900` (mando estándar),
  estado de botones y sticks del frontend; transferencias inmediatas y su interrupción.
- **EXI:** canal 0 dispositivo 1 = el chip de la BIOS: RTC (segundos desde 2000) y SRAM (64 bytes
  con valores por defecto válidos, checksum correcto); lectura de la fuente de la BIOS no se
  ofrece (no meta). Ranuras de tarjeta de memoria vacías. Transferencias inmediatas y DMA, y sus
  interrupciones.
- **AI:** control, volumen, contador de muestras del streaming (apagado), interrupción.
- **Interfaz DSP:** buzones CPU→DSP y DSP→CPU, registro de control (reset, halt, interrupciones de
  DSP, ARAM y AI), DMA de ARAM (en ambos sentidos, con su interrupción), DMA de audio del AI
  (bloques de 32 bytes consumidos a 32 kHz con su interrupción). **El DSP en sí:** un HLE mínimo
  que contesta el protocolo de arranque del DSP (el correo de la ROM del DSP y la carga del
  microcódigo que envía el SDK) y después el protocolo del microcódigo que use MKDD, lo justo
  para que el juego avance. Qué microcódigo es y qué espera se descubre ejecutando MKDD y se
  documenta; si con respuestas simples no avanza, el agente lo anota como bloqueo para el
  subproyecto 3 y busca la salida mínima (decisión documentada).

### FIFO gráfico (sin dibujar)

- El write-gather pipe acumula escrituras de 32 bytes en el FIFO de RAM; el CP lee de ese FIFO
  (registros de base, final, puntero de lectura/escritura, marcas de nivel alto/bajo y su
  interrupción, breakpoint) y también procesa listas de visualización (`CALL_DL`).
- El analizador conoce el tamaño de cada comando: NOP, `LOAD_CP_REG` (`0x08`), `LOAD_XF_REG`
  (`0x10`), `LOAD_INDX_A..D` (`0x20/28/30/38`), `CALL_DL` (`0x40`), `INVALIDATE_VTX_CACHE`
  (`0x48`), `LOAD_BP_REG` (`0x61`), y las primitivas (`0x80–0xB8`) cuyo tamaño sale de VCD/VAT
  (los registros del CP), que por eso se guardan.
- Responde a lo que el juego espera: tokens y "draw done" del PE (BP `0x47`/`0x48`/`0x45`, con
  sus interrupciones por PI), y la copia del EFB al XFB (BP `0x52`) escribe el color de borrado
  cuando la copia pide borrar (para que el XFB no quede con basura).
- Contadores por campo (bytes de FIFO, comandos por tipo, primitivas, vértices), que el runner
  imprime: son la prueba de que el juego dibuja.

### Runner y ezyemu

- `ezy_gc_runner <imagen> --fields N [--screenshot out.bmp] [--trace-unimplemented]`: arranca,
  corre N campos, imprime estadísticas (instrucciones, interrupciones por tipo, lecturas de disco,
  bytes del FIFO por campo, correos del DSP), y guarda el XFB. Ante algo sin implementar, se
  detiene con PC, palabra/dirección y las últimas instrucciones ejecutadas.
- ezyemu: `GameCubeSystem` (imagen `.iso`/`.gcm`), 640×480, mando de Xbox → mando de GameCube
  (A, B, X, Y, Start, Z = RB, L/R analógicos = gatillos, stick y stick C), teclado como en N64.

## Validación

- Pruebas unitarias del Gekko: por grupo de instrucciones, con valores del manual y casos borde
  (desbordamientos, acarreos, rotaciones con máscaras que dan la vuelta, divisiones por cero
  con el resultado que da el hardware, NaN y redondeo en precisión simple, cuantización de los
  paired singles con escala y tipo de GQR, excepciones con SRR0/SRR1/MSR exactos).
- Pruebas de cada periférico con el bus de prueba (como `tests/n64/flat_bus.hpp`).
- Un disco sintético generado en las pruebas (cabecera, FST, un DOL pequeño escrito con un
  ensamblador de prueba) que arranca, lee un archivo por DI y termina con un valor conocido.
- Con MKDD (local, fuera del repo): el criterio de la meta 1, y un registro en
  `docs/progress/` con lo que fue haciendo falta para que arranque.
- Las suites existentes siguen pasando.

## Lo que queda al constructor

- La forma del decodificador (tablas por opcode primario/extendido), los nombres de clases.
- La duración fija de una lectura de disco y de los DMA.
- Qué exactamente contesta el DSP HLE (se descubre con MKDD; documentarlo).
- Detalles del modo de video (NTSC entrelazado/progresivo) según lo que pida MKDD.
- Si hace falta ejecutar el apploader en vez del arranque HLE directo (decidir con lo observado y
  documentar).
