# Recordatorios / contexto entre sesiones

Este archivo es la memoria compartida del proyecto entre PCs y cuentas de Claude.
**Claude: léelo al empezar cada sesión y actualízalo al terminar** (qué se hizo, qué quedó pendiente).
Responder siempre en español; código, commits y PRs en inglés.

_Última actualización: 2026-11-07_

## Estado actual

- **GBA**: core terminado (keypad IRQ, RTC, open bus, prefetch, save states). PR #1 a `main`.
- **GB/GBC**: mergeado en `main` (PR #2). Pasa cgb-acid2, dmg-acid2, Blargg cpu_instrs/instr_timing, Mooneye boot_regs-cgb.
- **Precisión GB**: PPU pixel-FIFO, OAM DMA, retraso de TIMA, timing de interrupciones (PR #3, mergeado). Mooneye 54 → 80 por modelo.
- Specs y planes de diseño: `docs/superpowers/specs` y `docs/superpowers/plans`.
- `roms/pokemon-silver.gbc` arranca y llega a la intro de Oak.

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
- [ ] **Subproyecto 3: RDP**.
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
  - [ ] Z: rangos DZ, bits ocultos y modos transparente/interpenetrante/decal; Z en rectángulos.
  - [x] Blender de un ciclo: lectura de memoria (`image_read`), `force_blend`, mezcla en bordes
        sin desbordar cobertura, P/A/M/B completos y destinos de cobertura clamp/wrap/zap/save.
  - [ ] Blender: `cvg_x_alpha`,
        `alpha_compare`. Los bits ocultos de cobertura en RGBA5551 se asumen en 1.
  - [x] Triángulos con sombreado y textura (`0x0E/0x0F`): TEXEL0 × SHADE en el combinador.
  - [x] Combinador general de un ciclo: (A−B)·C+D en RGB y alfa con texel (incluido su alfa),
        shade, primitivo, entorno y constantes; el blender usa el alfa combinado y selecciona P.
  - [x] Modo de dos ciclos: el ciclo 0 del combinador alimenta COMBINED, TEXEL1 lee el tile siguiente
        y el blender mezcla dos veces (el primer ciclo sin dividir, p. ej. niebla).
  - [ ] Combinador: ruido, chroma key, K4/K5, fracciones de LOD (sin LOD) y el TEXEL1 del ciclo 1
        (debería ser el TEXEL0 del pixel siguiente; hoy usa el del pixel actual).

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
- Runners para probar ROMs: `ezy_gb_runner --screenshot`, `ezy_gba_runner --screenshot`.
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
