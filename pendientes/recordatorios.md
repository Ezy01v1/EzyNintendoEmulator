# Recordatorios / contexto entre sesiones

Este archivo es la memoria compartida del proyecto entre PCs y cuentas de Claude.
**Claude: léelo al empezar cada sesión y actualízalo al terminar** (qué se hizo, qué quedó pendiente).
Responder siempre en español; código, commits y PRs en inglés.

_Última actualización: 2026-11-12_

> **Última revisión (2026-11-12): precodificación del combinador descartada.**
> Precargar selectores, colores, clave y K4/K5 al cambiar órdenes del RDP fue
> correcto (seis imágenes y estados completos idénticos), pero empeoró CPU en
> dos de tres parejas: -4,50 %, +0,27 %, -2,15 % de reducción; mediana -2,15 %.
> Se restauró el código de producción de `70e5e64`. Quedan pruebas de cambios
> sucesivos de selectores, colores, clave, conversión y LOD. Siete suites pasan
> con el core restaurado: N64 202 casos / 62774 comprobaciones por modo. Runner y
> frontend Release reconstruidos; estado a 120 idéntico a la referencia. Informe:
> `docs/performance/2026-11-12-n64-combiner-predecode.md`.
> **Siguiente área medible:** escritura de píxeles opacos (~8,23 % de muestras
> exclusivas) y después muestreo de textura (~5,62 %). Mantener el mismo método
> de parejas y hashes completos; no repetir esta caché sin evidencia nueva.
>
> **Última revisión (2026-11-12): dos experimentos del combinador descartados.**
> Tabla de entradas: CPU favorable 4/6 parejas, mediana de reducción 0,94 %.
> Productos nulos: 4/6, mediana 0,86 %, con regresiones hasta 7,32 %. Ambos
> correctos pero sin ganancia consistente; no repetirlos. Los 24 estados completos
> e imágenes de carrera son idénticos. Se restauró el renderizador de `70e5e64`;
> quedan pruebas de selectores y `benchmark-race.ps1 -Reverse`. Compilación final
> Debug + siete suites en verde: N64 200 casos / 62762 comprobaciones por modo.
> Runner/frontend Release recompilados y estado final idéntico a la referencia.
> Informe: `docs/performance/2026-11-12-n64-combiner.md`.
>
> **Mejora previa conservada, rama `codex/n64-rsp-jit`: carrera de MK64 y RDP.**
> Medición real del tramo 1500–1900 con `--benchmark` (arranque excluido): el JIT
> no gana de forma consistente. El perfil señaló el RDP y su mapa de bits DZ.
> Se reemplazó por acceso directo de 4 MiB, manteniendo el formato sparse v11.
> Tres parejas alternadas mejoran CPU en 13,1–15,2 % (mediana 13,61 %); velocidad
> mediana **22,91 → 26,53 campos/s**, con imágenes y estados completos idénticos.
> Suite completa 7/7; cada combinación N64 pasa 197 casos / 62488 comprobaciones.
> Continuidad cruzada intérprete/JIT y RDP síncrono/en hilo pasa a 1900+120 campos.
> Informe: `docs/performance/2026-11-12-n64-race.md`; reproducción:
> `tools/n64_runner/README.md` y `benchmark-race.ps1`.
>
> **Siguiente experimento:** escritura de píxeles y texturas del RDP. En el
> perfil posterior de carrera: RDP ~35,7 %, RSP ~29,4 %, CPU ~16,5 %; mapas y
> `RtlAllocateHeap` ya no aparecen. Son muestras exclusivas aproximadas, no tiempos
> inclusivos ni prueba de una futura ganancia. Medir cada cambio en parejas, con
> intérprete y RDP síncrono, manteniendo la misma imagen y estado. No comparar
> tiempos entre lotes/PCs ni usar el tiempo de una ejecución perfilada como benchmark.
>
> **Experimentos ya evaluados:** VMADN/AVX2 correcto pero sin ganancia (CPU mediana
> +0,42 % intérprete / +1,86 % JIT); queda OFF. No ampliar a VMADM/VMADH sin medir
> primero un kernel mejor. Informe `docs/performance/2026-11-12-vmadn-validation.md`.
> Omitir entradas JIT no soportadas también se descartó, sin mejora consistente:
> `docs/performance/2026-11-11-rsp-execution.md`. No repetir ese ajuste.
> La caché de traducciones y las transferencias vectoriales nativas ya están hechas;
> detalles en `docs/performance/2026-11-11-rsp-vector-memory.md`.
>
> Mantener intérprete y RDP síncrono predeterminados; JIT y RDP en hilo siguen
> experimentales. No fusionar a main sin demostrar una ganancia consistente.
> La ROM histórica systemtest está en esta PC y la nueva ejecución con JIT/AVX2=OFF
> conserva 2984/3721 (737 fallos conocidos), sin regresiones tras el cambio del RDP.
> Una sola compilación o prueba pesada a la vez, sin Debug/Release en paralelo.

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
    frente al intérprete en tres pares fríos. Perfil posterior completado: RSP ~42 %
    de muestras, con código nativo ~6,3 %, entrada JIT ~3,3 % y thunk vectorial ~3,8 %.
    Omitir entradas no soportadas no mejoró de forma consistente y se retiró.
    Frecuencia vectorial medida: primer grupo nativo LBV/LSV/LLV/LDV y stores,
    con accesos agrupados y respaldo por bytes/helper. Conservado tras 12 procesos
    fríos: CPU favorable en 5/6 parejas, reducción mediana emparejada 8,77 % frente
    al JIT anterior; el tiempo real varía mucho. No demuestra mejora frente al
    intérprete. Siguiente: evaluar VMADN/VMADH/VMADM según coste; no repetir
    frecuencia, caché ni omisión de entradas como trabajo pendiente.
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
- 2026-11-11 — Rama `codex/n64-rsp-jit` publicada hasta `e7cc68f` con autorización
  del usuario y upstream configurado; sigue sin merge a main. `git pull --ff-only`
  al retomar confirmó que estaba al día. Perfil RIP comparativo de 300 campos MK64:
  intérprete 1595 muestras / JIT 1783; sin resolver 2,0 / 2,5 %. RSP agrupado
  41,6 / 41,9 %, RDP 28,3 / 25,4 %, CPU VR4300/COP0/COP1/TLB 10,1 / 9,5 %.
  Los tiempos del muestreador no son benchmarks; porcentajes propios, no inclusivos.
  Experimento: clasificación compartida de opcodes para evitar entrar al JIT antes
  de fallback. Pasó las siete suites y revisión independiente sin hallazgos.
  Base/candidato compilados con LTCG completo, mismo preset y sin cambios de flags.
  Seis ejecuciones frías alternadas favorecieron al candidato, pero el lote de orden
  complementario invirtió el resultado real. Doce imágenes exactas, mismos contadores
  de instrucciones y compilaciones. CPU mejor solo en 3/6 parejas, mediana emparejada
  +1,42 % de CPU; tiempo real −5,28 %, con cambios de −31 % a +89 %. No hay ganancia
  consistente: cambios de producción retirados, conservada la caché de `e7cc68f`.
  Informe y método en `docs/performance/2026-11-11-rsp-execution.md`; datos y scripts
  locales en `build/rsp-execution-*.json`, `build/rsp-entry-benchmark*.json` y
  `build/rsp-entry-analysis.json`. Siguiente: trabajo vectorial del RSP; no ampliar
  CPU JIT ni repetir omisión de entradas como mejora demostrada. Intérprete y RDP
  síncrono siguen predeterminados. ROM histórica systemtest aún ausente.
  Verificación final tras retirar el experimento: `ctest --preset debug` 7/7;
  189 casos / 3562 comprobaciones por combinación N64. Runner y frontend Release
  recompilados. Revisión independiente de las conclusiones sin hallazgos. Se
  conservan informe y notas; ningún cambio nuevo de producción.
- 2026-11-11 — Perfil de frecuencia vectorial MK64 (300 campos): histogramas
  intérprete/JIT idénticos, con imágenes exactas. VMADN 6.713.927, VMADH 5.313.818,
  VMADM 3.589.466. Transferencias simples de 2/4/8 bytes: 16.066.225 en conjunto.
  Se emiten LBV/LSV/LLV/LDV y sus stores desde la vista actual de registros, con
  respaldo al helper sin vista, DMEM con máscara 4095 y semántica exacta de elementos.
  Primera prueba RED: sin helper retiraba 0 en vez de 1; GREEN tras implementación.
  4096 combinaciones, helper C++ real y cambio de puntero en caché con registro 31.
  La primera versión solo por bytes no mejoró (CPU mediana 9,688 → 9,844 s);
  se refinó con word/dword para tramos que caben, guardando los cruces por bytes.
  Las siete suites pasan: N64 192 casos / 36344 comprobaciones por combinación.
  OFF: 179 / 1973 en ambos modos RDP; ON restaurado y runner/frontend Release
  recompilados. Imágenes exactas a 120/300/600 campos y estados completos entre
  backends, en ambas direcciones tras 300+120. Revisión técnica sin hallazgos.
  Dos lotes complementarios (12 procesos fríos), base y candidato JIT con LTCG
  completo, sin contadores ni compilaciones simultáneas: CPU favorable en 5/6
  parejas, reducción mediana emparejada 8,77 %. Medianas agrupadas CPU 15,172 /
  13,891 s; reales 26,623 / 20,490 s. Tiempo real muy variable: no prometer su
  reducción emparejada del 25,37 % como aceleración general. Mismos 1316 bloques,
  190375 reutilizaciones y cero vaciados. Se conserva la variante agrupada en el
  JIT experimental; intérprete/RDP síncrono predeterminados, sin merge a main.
  Informe: `docs/performance/2026-11-11-rsp-vector-memory.md`; datos locales en
  `build/vector-frequency-*.log`, `build/vector-native-jit*.json` y
  `build/vector-native-analysis.json`. Contadores temporales retirados. Siguiente:
  medir coste del grupo VMADN/VMADH/VMADM antes de ampliar la emisión nativa.
- 2026-11-11 — Trabajo VMADN interrumpido por cierre de la PC y pausado explícitamente
  por el usuario. Se añadió una microprueba reproducible por opcode y se eligió
  VMADN por frecuencia/coste. Hay una implementación AVX2 local con comprobación
  CPU/OS, objeto separado sin LTCG, respaldo escalar y pruebas nuevas; no está
  completada su validación ni se ha publicado. Prueba aislada RED contra kernel
  vacío y GREEN con implementación: 20480 comprobaciones. El despacho público y
  sus pruebas completas son cambios posteriores, aún sin ejecución verificada.
  Se guardaron estados escalares de referencia a 120/300/600 campos antes de
  conectar el despacho acelerado y datos sintéticos en `build/vmad-before.csv`.
  Últimas compilaciones: Debug falló con Lib.exe 1102 y aviso OutOfMemory;
  Release con LNK1201 al escribir el PDB. En ese momento C: tenía menos de 10 MB
  libres; se retiraron 145,5 MiB de caches regenerables .iobj/.ipdb dentro de
  build/default. No se repitió la compilación después ni se midió el candidato.
  Comprobación ligera tras la petición de pausa: archivos locales conservados,
  último commit/remoto `f22e0a7`; LastTest.log completo es anterior al trabajo
  VMADN. No atribuirle esas siete suites previas a la implementación nueva.
  Reanudar con recursos controlados, una tarea pesada a la vez. Pendientes:
  compilar y ejecutar pruebas públicas de selectores/alias/flags/división,
  verificar respaldo escalar y AVX2 desactivado, comparar los estados completos
  contra las referencias y medir el efecto global antes de conservar/publicar.
- 2026-11-11 — El usuario pidió publicar los cambios pendientes en la rama para
  continuar desde otra PC. Checkpoint WIP explícito, sin ejecutar nuevas pruebas
  ni compilaciones. Se incluyen kernel, despacho, pruebas, microbenchmark, scripts
  portables por ruta y guía `docs/performance/2026-11-11-vmadn-handoff.md`, con los
  errores y validaciones pendientes. Las referencias binarias se regeneran en la
  otra PC con AVX2=OFF; no se publican ROMs ni estados con memoria del juego.
  La pausa de trabajo pesado continúa; publicar no implica validar ni mergear.
- 2026-11-12 — Retomada la rama `codex/n64-rsp-jit` en la otra PC. Validado el
  checkpoint VMADN: OFF pasa las cuatro suites N64 (193 casos / 48632
  comprobaciones cada una); ON pasa las siete suites, con 194 / 69112 en cada
  combinación N64. Despacho AVX2 confirmado, 30 checksums del microbenchmark
  iguales y comparación automática añadida al script (incluida prueba de rechazo
  de un checksum alterado). Estados completos de MK64 iguales a 120/300/600,
  también JIT/AVX2 frente a intérprete escalar a 600; continuidad cruzada en ambos
  sentidos a 300+120 e imágenes de doce ejecuciones alternadas exactas. ROM
  histórica systemtest encontrada aquí: termina con JIT/AVX2 y conserva 2984/3721.
  Sin ganancia de CPU: medianas +0,42 % intérprete / +1,86 % JIT. AVX2 queda como
  experimento OFF por defecto; no ampliar a VMADM/VMADH ni activar JIT por defecto
  con este resultado. Informe `docs/performance/2026-11-12-vmadn-validation.md`.
  Configuración local restaurada a AVX2=OFF con frontend habilitado; runner,
  benchmark y ezyemu Release recompilados. Prueba final: kernel escalar y captura
  de MK64 exacta a 300 campos. Los ejecutables de ambas variantes siguen en build/.
- 2026-11-12 — Medición en carrera de MK64 (1500–1900) con nueva ventana del runner,
  CPU/tiempo real y script de parejas alternadas. JIT sin ventaja consistente:
  medianas 21,84 campos/s intérprete / 21,13 JIT en el primer lote. Perfilador
  opcional por muestreo Windows x64, ejecutado aparte del benchmark: RDP ~32 %,
  mapa DZ/árbol ~6,7 % y `RtlAllocateHeap` ~2,1 %. Bits DZ ahora en arreglo directo
  de 4 MiB, con contador y formato sparse v11 idéntico. Las tres parejas de
  comparación mejoran CPU en 15,25 / 13,61 / 13,06 %; medianas 22,91 → 26,53
  campos/s (+15,77 % de velocidad en esa escena). Imágenes y estados completos
  exactos a 1900 en todos los procesos y perfiles. Suite completa 7/7, N64
  197 casos / 62488 comprobaciones por combinación; referencias sparse antiguas,
  rangos, límites y estados malformados cubiertos. Continuidad cruzada a
  1900+120 pasa. Systemtest con JIT y AVX2=OFF termina y conserva 2984/3721.
  Perfil posterior sin muestras de mapa/árbol ni `RtlAllocateHeap`; siguiente
  foco medible: combinador, escritura de píxeles y texturas del RDP. Runner y
  frontend Release recompilados. Informe `docs/performance/2026-11-12-n64-race.md`;
  herramientas en `tools/n64_runner/README.md`. Valores predeterminados sin cambios.
- 2026-11-12 — Continuación del combinador: probadas tabla de referencias y
  simplificación de productos nulos. Dos lotes complementarios por candidato,
  seis parejas cada uno (24 procesos fríos, tramo 1500–1900). Ambos conservan
  imagen/estado, pero solo 4/6 parejas favorables: medianas CPU 0,94 % y 0,86 %,
  menores que la variación observada. Se retiran ambos cambios de producción;
  el código del renderer queda en `70e5e64`, con su mejora DZ previa intacta.
  Quedan tres pruebas de conformidad (274 comprobaciones): RGB/alfa, selectores
  especiales, dos ciclos, productos nulos y avance incondicional de ruido.
  `benchmark-race.ps1` añade `-Reverse` para el lote complementario. Restaurado
  el motor y recompilado: siete suites pasan, N64 200 / 62762 por combinación;
  runner/frontend Release reconstruidos y estado a 120 idéntico a la referencia.
  No se repitió systemtest porque el core final no cambia; su baseline verificado
  sigue siendo el de `70e5e64` (2984/3721). Informe y datos locales documentados
  en `docs/performance/2026-11-12-n64-combiner.md`. Siguiente hipótesis: decodificar
  parámetros uniformes por cambio de estado, con pruebas de invalidación/restore.
- 2026-11-12 — Continuación: precodificación de selectores y uniformes del
  combinador implementada experimentalmente, con reconstrucción al recibir
  órdenes y al cargar estado. Seis procesos fríos de MK64 (1500–1900) conservaron
  exactamente imágenes y estados completos; CPU por pareja -4,50 %, +0,27 % y
  -2,15 % de reducción, mediana -2,15 %. Se retiró la caché de producción; el
  renderizador vuelve al de `70e5e64`. Quedan dos pruebas nuevas de cambios
  sucesivos de selectores, colores, clave, conversión y LOD. Las siete suites
  pasan con el core restaurado: N64 202 / 62774 por modo. Runner y frontend
  Release reconstruidos; estado a 120 idéntico a la referencia. No se repitió systemtest porque el core final
  coincide con el baseline que obtuvo 2984/3721. Informe
  `docs/performance/2026-11-12-n64-combiner-predecode.md`; siguiente foco:
  escritura de píxeles opacos, luego muestreo de textura.
