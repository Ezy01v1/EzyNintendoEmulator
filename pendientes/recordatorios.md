# Recordatorios / contexto entre sesiones

Este archivo es la memoria compartida del proyecto entre PCs y cuentas de Claude.
**Claude: léelo al empezar cada sesión y actualízalo al terminar** (qué se hizo, qué quedó pendiente).
Responder siempre en español; código, commits y PRs en inglés.

_Última actualización: 2026-11-11_

> **Para retomar:** perfilar el coste de ejecución del JIT frente al intérprete.
> La caché de traducciones idénticas ya está implementada en `codex/n64-rsp-jit`:
> 191.691 → 1.316 compilaciones por 300 campos de MK64, con 190.375 reutilizaciones.
> Sigue experimental: mediana local 17,029 s JIT frente a 13,716 s intérprete,
> con mucha variación de tiempo real y CPU mediana casi igual (11,406 / 11,531 s).
> Fallo de memoria reportado corregido y cubierto por regresión. Mantener intérprete
> y RDP síncrono predeterminados. Diseño/plan JIT documentan pruebas y límites.
> CPU/RSP optimizados en `main` (PR #15). El RDP en hilo está implementado y validado
> en `codex/n64-threaded-rdp`, pero quedó experimental: resultó más lento. La rama es local.

## Estado actual

- **GBA**: core terminado (keypad IRQ, RTC, open bus, prefetch, save states). PR #1 a `main`.
- **GB/GBC**: mergeado en `main` (PR #2). Pasa cgb-acid2, dmg-acid2, Blargg cpu_instrs/instr_timing, Mooneye boot_regs-cgb.
- **Precisión GB**: PPU pixel-FIFO, OAM DMA, retraso de TIMA, timing de interrupciones (PR #3, mergeado). Mooneye 54 → 80 por modelo.
- Specs y planes de diseño: `docs/superpowers/specs` y `docs/superpowers/plans`.
- `roms/pokemon-silver.gbc` arranca y llega a la intro de Oak.
- **N64**: CPU, RSP y RDP completos en `main` (PRs #6–#14). n64-systemtest 2984/3721.
  **Mario Kart 64 llega a la pantalla de título** y se puede abrir en `ezyemu` con teclado o gamepad,
  pero va a ~22 % de la velocidad real (~13 cuadros de video por segundo de 60). Falta velocidad.

## Pendientes (en este orden, antes de empezar N64)

1. [x] **Optimizar rendimiento GB**: hecho (PR #4, mergeado). El PPU salta los dots sin eventos
       de los modos 0/1/2. Pokémon Silver: 24 % → 12 % de un núcleo (medido en la PC con Build Tools).
2. [x] **ROMs Mooneye que fallan**: `vblank_stat_intr-C` (PR #4); `hblank_ly_scx_timing-GS`,
       `lcdon_timing-GS` y `lcdon_write_timing-GS` con la recalibración del PPU (PR #5,
       mergeado). Mooneye 84/115. Lo que falla ya no es de timing: arranque de
       otros modelos, `unused_hwio`, MBC2 y MBC1 multicart. Detalles en `roms/README.md`.
3. [ ] **Probar Pokémon Silver a fondo**: guardado en el juego, RTC día/noche, glitches gráficos.
       La intro se ve bien en `ezy_gb_runner --screenshot`; falta jugarlo en `ezyemu` (necesita al usuario).
4. [ ] **GBA: probar juegos comerciales**. Hay que copiar dumps `.gba` propios a `roms/` (ignorado por git).
       Pokémon Emerald sirve para probar el RTC. Usar `ezy_gba_runner --screenshot`.
5. [x] (Opcional) La ventana VRAM del debugger se encima con la ventana Screen (posición fija).
       Ya resuelto en `f82824b`: VRAM va en y=410, debajo de Screen (y=10..400).

## Nintendo 64 (en curso)

Plan en 4 subproyectos (spec y plan en `docs/superpowers/`): 1 CPU+memoria+arranque+video,
2 RSP, 3 RDP, 4 juegos (audio, guardado, rendimiento).

- [x] **Subproyecto 1** (PR #6, mergeado): VR4300 (COP0, TLB, FPU), bus, PI/SI/VI/AI/MI, PIF y
      arranque HLE con el IPL3 real, frontend (`.z64/.v64/.n64`) y `ezy_n64_runner`.
      PeterLemon 91/94, n64-systemtest base 2659/3721 (lo que falla está en `roms/README.md`).
- [ ] Pendientes del subproyecto 1 que siguen abiertos: FPU en modo FR=0 con índices impares y
      aritmética aleatorizada, escrituras de 8/16 bits a memoria del RCP, PI write latch, RE, MI repeat.
- [x] **Subproyecto 2: RSP**.
  - [x] Núcleo escalar y DMA de SP. Un programa cargado por DMA ejecuta hasta `BREAK` y puede
        interrumpir al VR4300. Pasan las pruebas escalares, de registros SP y de DMA de
        `n64-systemtest` (base 2824/3721). El timing de ejecución y DMA es aproximado.
  - [x] Unidad vectorial (COP2, acumulador de 48 bits por carril, banderas, recíproco y accesos
        vectoriales). Pasan todos los casos vectoriales de la ROM base (2977/3721 en total).
- [x] **Subproyecto 3: RDP** (PRs #7–#13, mergeados).
  - [x] Interfaz DPC y listas desde RDRAM o DMEM, relleno de rectángulos con recorte e
        interrupción de `Sync Full`. Pasan los siete casos básicos del RDP en `n64-systemtest`
        (2984/3721 en total).
  - [x] Triángulos planos sin textura en modo `FILL` (`0x08`). Se validaron las dos orientaciones
        del borde mayor, el recorte y la reanudación de un comando partido.
  - [x] Triángulos de color sólido en modo de un ciclo. Ruta opaca para RGBA5551/RGBA8888,
        ocho muestras de cobertura y tramado RGB por matriz.
  - [x] Carga RGBA5551 en TMEM con `Load Tile` y `Load Block`, y rectángulos texturizados
        en modo `COPY` con transparencia por alfa.
  - [x] Texturas CI4 y CI8 con `Load TLUT` en modo `COPY`. La paleta se guarda en TMEM y
        se restaura junto al resto del estado del RDP.
  - [x] Rectángulos opacos de un ciclo con muestreo puntual de RGBA5551, CI4 y CI8.
        Color directo o modulado por el color primitivo, con recorte y cobertura subpíxel.
  - [x] Filtrado de tres muestras en rectángulos opacos de un ciclo para RGBA5551, CI4 y CI8.
        Promedio de cuatro texeles en el centro con `G_TF_AVERAGE`.
  - [x] Triángulos opacos texturizados (`0x0A`) de un ciclo con interpolación afín, muestreo
        puntual y filtrado de tres muestras. Los estados guardados conservan el comando parcial.
  - [x] Corrección de perspectiva en triángulos opacos de un ciclo mediante S, T y W.
        La división usa el recíproco cuantizado del RDP antes de aplicar el muestreo.
  - [x] Triángulos con sombreado Gouraud (`0x0C`) de un ciclo: color por vértice interpolado,
        con saturación de 9 bits, cuando el combinador elige shade como color.
  - [x] Z-buffer en triángulos (`0x09/0x0B/0x0D/0x0F`) y profundidad primitiva: modo opaco,
        Z de 18 bits comprimido a 14. Estados guardados versión 9.
  - [x] Z: rangos DZ, bits ocultos de DZ y modos transparente/interpenetrante/decal.
  - [x] Blender de un ciclo: lectura de memoria (`image_read`), `force_blend`, mezcla en bordes
        sin desbordar cobertura, P/A/M/B completos y destinos de cobertura clamp/wrap/zap/save.
  - [x] Blender: `cvg_x_alpha`, `alpha_cvg_sel` y `alpha_compare` (umbral fijo y aleatorio).
  - [x] Triángulos con sombreado y textura (`0x0E/0x0F`): TEXEL0 × SHADE en el combinador.
  - [x] Combinador general de un ciclo: (A−B)·C+D en RGB y alfa con texel (incluido su alfa),
        shade, primitivo, entorno y constantes; el blender usa el alfa combinado y selecciona P.
  - [x] Modo de dos ciclos: el ciclo 0 del combinador alimenta COMBINED, TEXEL1 lee el tile siguiente
        y el blender mezcla dos veces (el primer ciclo sin dividir, p. ej. niebla).
  - [x] Combinador: ruido, chroma key, K4/K5, fracciones de LOD y TEXEL1 del ciclo 1 (pixel siguiente).
  - [x] Mipmapping (LOD) en triángulos y rectángulos, con texturas sharpen y detail.
  - [x] `Fill Rectangle` en modos de 1 y 2 ciclos pasa por combinador y blender (fundidos de MK64).
  - [ ] Detalles menores del RDP: shift del blender en modo interpenetrante, bits ocultos de
        cobertura en RGBA5551 (se asumen en 3) y de escrituras de la CPU, K0–K3 (YUV).

## Subproyecto 4: velocidad N64 (siguiente)

Medido con Mario Kart 64 (Release, `ezy_n64_runner --screenshot ... 300`): **~23 s para 300 cuadros**
(a velocidad real serían 5 s). Perfil por función (muestreador temporal, ya quitado):
CPU ~40 % (incluye el trabajo fijo por instrucción en `Vr4300::step`), RSP ~22 %, RDP ~12 %,
planificador ~12 %. Ya hecho (PR #14): LTO + PDB en Release, próximo evento en caché, frontend con
máx. 2 cuadros por dibujo. Probado y **descartado**: correr el RSP por lotes con retraso
(no aceleró y colgó una prueba).

Meta: 60 cuadros de video por segundo (100 % de velocidad); MK64 se vería a sus 30 fps originales.
Medir con MK64 después de cada cambio y mantener n64-systemtest en 2984/3721.

- [ ] **Etapa 1: afinar el intérprete** (meta ~25–30 cuadros/s):
  - [x] CPU (rama `n64/cpu-decode-cache`): **el 91 % de las instrucciones de MK64 eran el bucle
        ocioso de libultra** (`b .` + `nop` en 0x5B8). Ahora se salta hasta el próximo evento o
        Compare, exacto al ciclo. Además: RDRAM directo en load/store, vía rápida del RSP en el
        planificador, COP0 en línea. MK64 300 cuadros: 20,1 s → ~15,5 s, imagen idéntica.
        Una caché de decodificación **no** sirve: decodificar MIPS es barato (medido).
  - [x] Bucle ocioso mientras el RSP trabaja: RSP, RDP y DMA de SP corren en lote durante la
        espera (sus manejadores no leen `now()`; AI sí, por eso corta el lote). Se corta en el primer
        evento que deja una interrupción pendiente y la CPU retoma en ese ciclo con la paridad
        correcta (salto o `nop`). MK64 300 cuadros: ~15,5 s → **~9 s** (2,2× desde 20,1), imagen idéntica.
  - Perfilador: `prof.cpp` (muestrea el RIP cada 1 ms y resuelve con el PDB); no está en el repo.
  - [x] RSP vectorial: cada opcode es una plantilla (`run_lanes<op>`) despachada por tabla; sin
        `switch` por carril. MK64 ~9 s → ~7,9 s. Cada op pesa ahora ~1,5 %: SSE a mano no vale la pena.
  - [x] RSP con reloj local (`RspRegs::run`): corre tramos sin pasar por el planificador en cada
        instrucción; relee `others_bound()` porque el propio RSP agenda DMA/RDP. MK64 ~7,9 s → ~7,3 s.
  - [x] Ruta rápida de profundidad del RDP: los píxeles transparentes y los opacos con overflow
        o profundidad máxima evitan consultar DZ cuando no afecta al resultado. En esta PC,
        dos pares de pruebas Release de 300 cuadros: CPU media 15,656 s → 15,164 s (~3 % menos);
        tiempo real medio 19,706 s → 19,491 s, con variación entre ejecuciones. Imagen final idéntica.
        Sigue lejos del tiempo real; no confundir estas mediciones con las de la otra PC.
  - [x] RDP en otro hilo, **experimental** en `codex/n64-threaded-rdp`: FIFO, captura de
        comandos por valor y barreras de RDRAM por página, también en CPU directa, DMA y VI.
        `Sync Full` y estados v11 conservados. 176/176 pruebas en cada modo; MK64 con imágenes
        idénticas a 120/300/600 cuadros y continuación de estado cruzada 300+120.
        Tres mediciones por modo: mediana síncrona **11,311 s**, hilo **12,961 s** por 300 cuadros
        (**14,6 % más lento**); CPU mediana 10,281 s vs 14,031 s, incluyendo ambos hilos.
        No cumplió el umbral de mejora del 10 %: frontend y core siguen síncronos por defecto.
        Runner: `--rdp-threaded`, `--rdp-sync`, `--state-check 300`.
        Diseño y plan en `docs/superpowers/`, con fecha 2026-11-10. No repetir esta implementación
        como pendiente ni confundir estas mediciones locales con las de otra PC u otro momento.
- [ ] **Etapa 2: decidir según lo medido**:
  - Perfil local actualizado: `execute_scalar` 11,7 %, bucle RSP 6,7 %, memoria vectorial
    5,9 %, ALU vectorial 5,7 %, más operaciones por carril. **JIT del RSP implementado**
    y validado localmente, con AsmJit fijado por SHA, invalidación y presupuesto exacto.
    El intérprete sigue predeterminado. La primera entrega del JIT fue 4,10 veces
    más lenta por compilaciones. La caché de traducciones idénticas ya está hecha
    (2026-11-11): reduce las compilaciones a 1.316, pero no demuestra mejora global
    frente al intérprete en tres pares fríos. Siguiente: perfilar ejecución nativa,
    helpers y salidas de bloque; no repetir la caché como trabajo pendiente.
  - Experimentos descartados: `/Ob3` (+0,15 % mediano, no significativo) y despacho vectorial
    directo desde `run` (4,9 % más lento). Ambos revertidos; no repetirlos como mejoras.
  - Dynarec (JIT) MIPS → x86-64 para la CPU: alternativa futura que requiere perfil
    y medición de juego completo; no asumir un multiplicador de velocidad.
  - O RSP en alto nivel (HLE) de los microcódigos gráficos (F3DEX etc.) si el RSP sigue pesando.
- [ ] Más adelante: RDP en la GPU (estilo parallel-RDP con compute shaders).
- Nota GameCube/Wii: necesitarán JIT y gráficos en la GPU desde el principio (diseñarlo así).

## Roadmap

GBA (terminar pendientes) → **N64** → Nintendo DS → GameCube → Wii.
Wii U / Switch solo después, si vale la pena. **No** NES ni SNES.

## Cómo trabajar

- En la PC con solo Build Tools 2022 (sin Visual Studio completo), `cmake` no está en el PATH: viene en
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`.
- ROMs de prueba: todas en `roms/` (de dónde salen: `roms/README.md`). Datos SingleStepTests:
  `git clone --depth 1 https://github.com/SingleStepTests/sm83.git testdata/sm83`.
- Uso varios PCs: el código se mueve solo por git (`git pull` al empezar, `git push` al terminar).
- Compilar solo con presets: `cmake --preset default` → `build/default`,
  `cmake --build --preset debug`, `ctest --preset debug`.
  Si CMake se queja de rutas, borrar `build/default` y reconfigurar.
- Runners para probar ROMs: `ezy_gb_runner --screenshot`, `ezy_gba_runner --screenshot`,
  `ezy_n64_runner <rom> --screenshot out.bmp <cuadros>` (usar el de `Release` para medir velocidad).
- Mario Kart 64 del usuario: `E:\Rom nintendo\Mario Kart 64 (USA).z64` (fuera del repo, no subir).
  En esta PC: `roms/Mario Kart 64 (USA).z64`, extraída del ZIP que dejó el usuario en la raíz.
  SHA-1: `579c48e211ae952530ffc8738709f078d5dd215e`, idéntico a la versión USA de
  https://github.com/n64decomp/mk64. La lentitud no se debe a una ROM corrupta.
  El ZIP de la raíz también es una ROM: no agregarlo a ningún commit.
  Abrirlo: `build\default\src\frontend\Release\ezyemu.exe "E:\Rom nintendo\Mario Kart 64 (USA).z64"`
  (después de `cmake --build --preset release`). Controles N64: Enter=Start, Z=A, X=B, Q=Z,
  A/S=L/R, IJKL=stick, flechas=cruceta, T/G/F/H=botones C; también gamepad.
- Las ROMs van en `roms/` y no se suben al repo.

## Bitácora

- 2026-11-05 — GBA terminado, PR #1. Se agregaron CMake presets.
- 2026-11-06 — GBC mergeado (PR #2). Pase de precisión GB mergeado (PR #3). Se crea esta carpeta.
- 2026-11-06 — Revisión de pendientes: el 5 (ventana VRAM) ya estaba resuelto en `f82824b`; se marca. 1–4 siguen abiertos.
- 2026-11-06 — PC con Build Tools: se instalan Build Tools y ROMs de prueba. Pendiente 1 hecho (24 % → 12 %), `vblank_stat_intr-C` arreglado. hblank_ly_scx y lcdon siguen abiertos. PR #4 mergeado.
- 2026-11-06 — Recalibración del PPU (LY 4 dots antes, bloqueos de bus anticipados, modo 3 de 172): pasan hblank_ly_scx y lcdon*, Mooneye 81 → 84. PR #5 mergeado.
- 2026-11-07 — N64 subproyecto 1 (rama `n64/base`): PeterLemon 91/94, n64-systemtest 2659/3721. Siguiente: RSP. PR #6 mergeado.
- 2026-11-07 — N64 subproyecto 2: núcleo escalar y DMA de SP. Pruebas unitarias N64 119/119,
  PeterLemon 91/94 y n64-systemtest 2824/3721. Siguiente: unidad vectorial.
- 2026-11-07 — N64 subproyecto 2: unidad vectorial integrada. Pruebas unitarias N64 121/121 y
  n64-systemtest 2977/3721. Siguiente: RDP.
- 2026-11-07 — N64 subproyecto 3: interfaz DPC, rectángulos y sincronización. Pasan los siete
  casos básicos de RDP de n64-systemtest. Siguiente: triángulos sin textura.
- 2026-11-07 — N64 subproyecto 3: triángulos planos en modo `FILL`, con recorte y guardado de
  comandos parciales. Pruebas unitarias N64 126/126. Siguiente: color sólido en modo de un ciclo.
- 2026-11-07 — N64 subproyecto 3: triángulos opacos en modo de un ciclo, cobertura y color.
  Pruebas unitarias N64 129/129. Siguiente: rectángulos texturizados en modo `COPY`.
- 2026-11-07 — N64 subproyecto 3: TMEM y rectángulos RGBA5551 en modo `COPY`. Pruebas
  unitarias N64 131/131, casos PeterLemon registrados en CTest 91/91 y n64-systemtest
  2984/3721. Siguiente: CI4/CI8 y TLUT.
- 2026-11-07 — N64 subproyecto 3: rectángulos CI4 y CI8 con paletas TLUT en modo `COPY`.
  Pruebas unitarias N64 133/133 y n64-systemtest 2984/3721. Siguiente: rectángulos
  texturizados en modo de un ciclo.
- 2026-11-07 — N64 subproyecto 3: rectángulos texturizados de un ciclo con muestreo puntual.
  Pruebas unitarias N64 135/135 y n64-systemtest 2984/3721. Siguiente: filtrado de tres muestras.
- 2026-11-07 — N64 subproyecto 3: filtrado de tres muestras en rectángulos de un ciclo.
  Pruebas unitarias N64 137/137 y n64-systemtest 2984/3721. Siguiente: triángulos opacos texturizados.
- 2026-11-07 — Triángulos texturizados opacos con interpolación afín en el RDP.
  Pruebas unitarias N64 140/140 y n64-systemtest 2984/3721.
- 2026-11-07 — Corrección de perspectiva con S, T y W en triángulos opacos del RDP.
  Pruebas unitarias N64 142/142 y n64-systemtest 2984/3721.
- 2026-11-07 — Trabajo de RSP/RDP pasado a la rama `n64/rsp-rdp` (estaba sin commit en `main`).
  Triángulos Gouraud de un ciclo. Pruebas unitarias N64 143/143. Siguiente: Z-buffer.
- 2026-11-07 — N64 subproyecto 3: Z-buffer opaco en triángulos. Pruebas unitarias N64 144/144,
  n64-systemtest 2984/3721. Siguiente: mezcla con el framebuffer o shade con texturas.
- 2026-11-07 — N64 subproyecto 3: blender de un ciclo con lectura del framebuffer. Pruebas
  unitarias N64 145/145, n64-systemtest 2984/3721. Siguiente: shade con texturas.
- 2026-11-07 — N64 subproyecto 3: triángulos con textura modulada por shade. Pruebas unitarias
  N64 146/146, n64-systemtest 2984/3721. Siguiente: combinador general.
- 2026-11-07 — N64 subproyecto 3: combinador general de un ciclo en lugar de los casos fijos.
  Pruebas unitarias N64 148/148, n64-systemtest 2984/3721. Siguiente: modo de dos ciclos.
- 2026-11-07 — N64 subproyecto 3: modo de dos ciclos en combinador y blender. Pruebas unitarias
  N64 150/150, n64-systemtest 2984/3721.
- 2026-11-08 — PR Ezy01v1/EzyNintendoEmulator#7 (n64/rsp-rdp: RSP y rasterizador RDP) mergeado a main.
- 2026-11-08 — N64 subproyecto 3: alpha compare (umbral fijo), cvg_x_alpha y alpha_cvg_sel en el blender. Pruebas unitarias
  N64 152/152, n64-systemtest 2984/3721. Pendiente: alpha compare con umbral aleatorio, modos Z, chroma key, ruido.
- 2026-11-08 — PR #8 (alpha compare) mergeado. N64 subproyecto 3: modos Z transparente, interpenetrante y decal (sin DZ);
  Z solo se escribe si el blender escribe el pixel. Pruebas unitarias en verde, n64-systemtest 2984/3721.
  Pendiente: rangos DZ, ajuste de cobertura del modo interpenetrante, chroma key, ruido.
- 2026-11-08 — N64 subproyecto 3: rangos DZ (DZ del pixel por pendientes o prim depth, código de 4 bits, ensanche por
  precisión, overflow de cobertura). Pruebas unitarias en verde, n64-systemtest 2984/3721.
  Pendiente: bits DZ ocultos (se toman como 0), ajuste de cobertura del modo interpenetrante, chroma key, ruido.
- 2026-11-08 — PR #10 (modos Z y DZ) mergeado. N64 subproyecto 3: Set Key R/GB, KEY_CENTER/KEY_SCALE y chroma key
  en el combinador (alfa = margen mínimo), ruido determinista (LCG) y alpha compare con umbral aleatorio.
  Save state v10. Pruebas unitarias en verde, n64-systemtest 2984/3721.
  Pendiente: K4/K5 (convert), fracciones de LOD, TEXEL1 del pixel siguiente, bits DZ ocultos, cobertura interpenetrante.
- 2026-11-08 — N64 subproyecto 3: Set Convert (K4/K5) y PRIM_LOD_FRAC de Set Primitive Color en el combinador.
  LOD_FRACTION sigue en 0 hasta tener mipmapping. Pruebas unitarias en verde, n64-systemtest 2984/3721.
- 2026-11-08 — N64 subproyecto 3: mipmapping en triángulos (LOD por pixel desde el paso de S/T, tile por nivel,
  LOD_FRACTION, nivel mínimo y máximo). Pruebas unitarias en verde, n64-systemtest 2984/3721.
  Pendiente: LOD en rectángulos, sharpen y detail, TEXEL1 del pixel siguiente, bits DZ ocultos, cobertura interpenetrante.
- 2026-11-08 — N64 subproyecto 3: texturas sharpen (fracción negativa al ampliar) y detail (tile de detalle + cadena
  desplazada). Pruebas unitarias en verde, n64-systemtest 2984/3721.
  Pendiente: LOD en rectángulos, TEXEL1 del pixel siguiente, bits DZ ocultos, cobertura interpenetrante.
- 2026-11-08 — N64 subproyecto 3: LOD en rectángulos texturizados (paso constante DsDx/DtDy, nivel máximo 0, sharpen
  y detail). Pruebas unitarias en verde, n64-systemtest 2984/3721.
  Pendiente: TEXEL1 del pixel siguiente, bits DZ ocultos, cobertura interpenetrante.
- 2026-11-08 — N64 subproyecto 3: en dos ciclos, el TEXEL1 del ciclo 1 es el TEXEL0 del pixel siguiente (triángulos y
  rectángulos). Pruebas unitarias en verde, n64-systemtest 2984/3721.
  Pendiente: bits DZ ocultos, cobertura interpenetrante.
- 2026-11-08 — PR #11 mergeado. N64 subproyecto 3: bits ocultos de RDRAM para los 2 bits bajos de DZ (mapa disperso,
  save state v11; los fills los borran) y ajuste de cobertura del modo Z interpenetrante. Pruebas unitarias en verde,
  n64-systemtest 2984/3721. ROM para probar: Mario Kart 64 en E:\Rom nintendo (fuera del repo).
  Pendiente RDP: shift del blender en interpenetrante, bits ocultos de color 5551 y de escrituras de CPU.
- 2026-11-08 — PR #12 mergeado. Mario Kart 64 llega a la pantalla de título (runner sin ventana, Release ~0,1 s/frame).
  Fill Rectangle en 1 y 2 ciclos ahora pasa por combinador y blender (los fundidos a negro de MK64 se descartaban).
  Las estelas del logo giratorio parecen un efecto del juego (sin confirmar contra hardware).
  Siguiente: ventana con imagen y control para N64, y velocidad.
- 2026-11-08 — Frontend: soporte de gamepad (SDL3) en la interfaz System y mapeo al mando de N64 (stick izquierdo,
  C con stick derecho, LT=Z, RB/RT=R, LB=L). La ventana y el teclado de N64 ya existían. Pendiente: probar MK64 jugando
  (va lento, ~1/6 de tiempo real) y acelerar la emulación.
- 2026-11-08 — Velocidad N64: perfil de MK64 (CPU ~40% con el bucle de pasos, RSP ~22%, RDP ~12%, planificador ~12%).
  Release con LTO y PDB, próximo evento del planificador en caché, frontend con máx. 2 frames por dibujo.
  300 frames de MK64: 29 s -> 23 s (sigue ~4x más lento que tiempo real). Probé dejar que el RSP corra por lotes
  con retraso: no aceleró y colgó una prueba; descartado. Siguiente: decodificación en caché de la CPU, SIMD en el RSP.
- 2026-11-08 — PR #14 mergeado (gamepad + velocidad). Recordatorios al día: RDP marcado como terminado y plan
  de velocidad N64 en etapas (Subproyecto 4). Próxima sesión: Etapa 1 (intérprete en caché, SIMD en RSP, RDP en hilo).
- 2026-11-09 — Velocidad N64, CPU (rama `n64/cpu-decode-cache`): salto del bucle ocioso exacto al ciclo, RDRAM directo,
  vía rápida del RSP en el planificador. MK64 300 cuadros 20,1 s → ~15,5 s con imagen idéntica. 154/154 pruebas,
  n64-systemtest 2984/3721. Siguiente: RSP en lote durante el bucle ocioso, luego SIMD en el RSP.
- 2026-11-09 — RSP/RDP en lote durante el bucle ocioso de la CPU, cortando exacto en la interrupción. MK64 300 cuadros
  ~9 s (antes 20,1 s), imagen idéntica, 154/154 pruebas, n64-systemtest 2984/3721. Ahora pesa el RSP
  (vector multiply/alu) y el RDP. Siguiente: SIMD en `RspVector`.
- 2026-11-09 — RSP vectorial por plantillas de opcode: MK64 300 cuadros ~7,9 s (≈38 cuadros/s), imagen idéntica,
  154/154 pruebas, n64-systemtest 2984/3721. Siguiente: RSP escalar con reloj local, o RDP en otro hilo.
- 2026-11-09 — RSP con reloj local: MK64 300 cuadros ~7,3 s (≈41 cuadros/s; empezó el día en 20,1 s), imagen idéntica,
  154/154 pruebas, n64-systemtest 2984/3721. Siguiente: abrir PR de `n64/cpu-decode-cache`; luego RDP en otro hilo o JIT.
- 2026-11-10 — PC local sincronizada con PR #15. ROM de MK64 extraída a `roms/` y SHA-1 verificado contra
  n64decomp/mk64: coincide con USA. Compilados frontend y runner Release. Perfil temporal en `build/`:
  RSP escalar ~13 %, ejecución local RSP ~6 %, depth_test del RDP ~7 %, N64::step ~7 % (tiempo propio).
  Ruta rápida de Z opaco/transparente: CPU media de 300 cuadros 15,656 → 15,164 s (~3 % menos),
  tiempo real 19,706 → 19,491 s (~15 cuadros/s en esta PC), comparaciones BMP exactas en todas las ejecuciones.
  Suite N64 Debug en verde con `ctest --preset debug -R '^ezy_n64_tests$'`.
  No se volvió a ejecutar n64-systemtest: su ROM no está disponible en esta PC.
  La lentitud sigue pendiente; siguiente: optimización mayor de RSP/RDP (hilo o HLE/JIT según perfil).
- 2026-11-10 — Preparado el diseño de RDP en otro hilo tras el visto bueno para continuar.
  Revisión técnica sin preguntas importantes pendientes. CPU conserva DPC, tiempo e interrupciones;
  trabajador dueño del rasterizador, con reservas por página y barreras también en la RAM directa
  del VR4300, DMA, VI y estados guardados. No se implementó todavía: falta aprobar el diseño.
  La autorización para publicar en `main` continúa pendiente; no se reintentó el push rechazado.
- 2026-11-10 — Implementado el RDP en hilo en `codex/n64-threaded-rdp`: trabajador ordenado,
  reservas por páginas y separación de DPC/rasterizador. Revisión técnica detectó un borde inclusivo
  de COPY/fill que podía dejar una página sin reservar; corregido con regresión RED→GREEN.
  176 casos y 1594 comprobaciones pasan tanto en síncrono como con hilo. MK64: BMP exactos a
  120/300/600 cuadros y estados cruzados con continuación 300+120 idénticos.
  Medición alternada (tres por modo): 11,311 s síncrono vs 12,961 s hilo; el hilo tarda 14,6 % más.
  141724 paquetes y ~62000–65000 esperas por 300 cuadros, sin saturar la cola. Se conserva síncrono
  por defecto y el hilo queda experimental. No se ejecutaron n64-systemtest (ROM ausente) ni TSan
  (sin toolchain compatible preparado). Siguiente: nuevo perfil y decisión de Etapa 2, no cambiar ROM.
  Sin merge ni push; la publicación sigue requiriendo autorización explícita.
- 2026-11-10 — Etapa 2, rama `codex/n64-rsp-speed`: perfil síncrono actualizado. `/Ob3` comparado
  sin caché incremental LTCG: mediana 12,692 → 12,673 s, sin ganancia real clara; retirado.
  Despacho directo COP2/LWC2/SWC2: 11,561 → 12,124 s, 4,9 % peor; retirado. Tres pares alternados
  por experimento, imagen exacta. No quedó ninguno de los cambios de rendimiento en producción.
  Se conserva una prueba diferencial del RSP en 41 límites de ciclo, con r0, delay slot vectorial,
  memoria, COP2, evento externo y BREAK con IRQ: 177/177 casos y 1963 comprobaciones en ambos
  modos RDP. Las marcas de hora de slots inactivos del planificador no son observables; la prueba
  compara eventos pendientes/próximo evento y todo el estado RSP/MI, no esos residuos históricos.
  Preparado y revisado diseño JIT del RSP, sin preguntas técnicas importantes pendientes; falta
  aprobación antes de instalar AsmJit o implementar. La ROM histórica de systemtest es `196f542`;
  su repo actual remite a nemu64-test, cuyos nuevos totales no deben confundirse con 3721.
- 2026-11-10 — JIT opcional del RSP implementado en `codex/n64-rsp-jit` con AsmJit
  `677a10bac2c24f7e068cf1f9e675bf51c7b534ea`, bloques de hasta 32 instrucciones,
  fallback y estados compatibles. Corregida la lectura nula de la captura: reset suave
  del allocator conservaba enlaces obsoletos; reset duro entre bloques nativos y regresión.
  Cuatro suites Debug: 187 casos / 2857 comprobaciones cada una; OFF: 179 / 1973 en
  ambos modos RDP, sin AsmJit. ON restaurado y runner/frontend Release recompilados.
  Imágenes exactas MK64 a 120/300/600 campos y estados entre backends. Tres pares fríos:
  mediana 11,139 s intérprete vs 45,708 s JIT, CPU 9,500 vs 39,391 s. JIT compila
  191691 bloques por 300 campos (33,660 s medianos), con 71,2 % de instrucciones nativas.
  Queda experimental: la lentitud sigue pendiente. Evaluar reutilizar traducciones de
  microcódigo idéntico; no repetir el JIT actual como mejora demostrada. ROM histórica
  systemtest ausente, baseline sin verificar. Revisión técnica completada, sin merge/push.
- 2026-11-11 — Continuación autorizada: caché histórica del JIT por PC y ventana exacta
  de hasta 32 instrucciones, además de la tabla rápida por generación. Invalidar IMEM
  desconecta la tabla rápida; agotar los 16 MiB de código hace reset duro y limpia
  ambas tablas. Las claves no se serializan y solo se retienen compilaciones exitosas.
  Regresión RED: A/B/A/B compilaba 4 bloques en vez de 2; múltiples generaciones,
  672 en vez de 224. GREEN tras el cambio. Cobertura de cambios internos y del último
  opcode, PC distinto con bytes iguales, final de IMEM, presupuesto y allocator.
  Revisión técnica independiente sin hallazgos. Imágenes MK64 exactas a 120/300/600
  campos y estados completos cruzados en ambas direcciones tras 300+120 campos.
  Tres procesos fríos por backend, alternados y sin compilaciones simultáneas:
  intérprete 16,678 / 11,164 / 13,716 s; JIT 30,099 / 17,029 / 12,182 s.
  Medianas de tiempo real 13,716 / 17,029 s (JIT 24,16 % más lento en este lote);
  CPU 11,531 / 11,406 s. La variación es alta: no demuestra ganancia global del 10 %.
  Por 300 campos: 1.316 compilaciones, 190.375 reutilizaciones, ningún vaciado;
  compilación mediana 0,750 s, mismas 76.828.052 instrucciones nativas / 31.073.143
  interpretadas. Datos en `build/rsp-cache-benchmark.json`; script local de medición
  `build/benchmark-rsp-cache.ps1`. Mantener intérprete y RDP síncrono por defecto.
  La ROM histórica systemtest sigue ausente. Sin merge/push ni ROMs en commits.
  Verificación final: `ctest --preset debug` 7/7 suites; N64 189 casos / 3562
  comprobaciones por combinación. OFF: 179 / 1973 en ambos modos RDP, sin AsmJit.
  ON restaurado y runner/frontend Release recompilados. La rama sigue local;
  `git pull --ff-only` no aplicó cambios porque no tiene upstream configurado.
