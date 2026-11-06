# Diseño: pasada de precisión del Game Boy (PPU con FIFO de píxeles)

Fecha: 2026-11-06 · Rama: `gb/accuracy` (incluye el commit de `gb/debug-layout`)

## Intención y por qué

El usuario está construyendo el emulador de forma progresiva, con la meta de llegar a
GameCube y Wii. Más adelante evaluará si vale la pena intentar consolas mucho más exigentes
(Wii U, Switch). Este proyecto es un paso en ese camino: emular el Game Boy tal como funciona
el hardware, ciclo a ciclo y pixel a pixel, en vez de con atajos. Que esto sirva de base para
las consolas complejas es una inferencia del asistente, no algo que el usuario haya dicho.

El usuario dejó en manos del asistente cómo convive el PPU nuevo con el actual y el alcance.
Aprobó el alcance propuesto.

## Objetivos

1. **PPU con FIFO de píxeles** (DMG y CGB) que **reemplaza** al dibujo por líneas
   (`render_scanline` y la duración estimada del modo 3):
   - Fetcher de fondo/ventana y FIFO de fondo, FIFO de sprites, búsqueda en OAM en el modo 2,
     descarte de `SCX % 8` píxeles, activación de la ventana a mitad de línea y las pausas
     del fetcher por cada sprite. La duración del modo 3 **sale sola** de esta simulación;
     ya no se estima.
   - Los registros se leen en el momento en que el hardware los usa, así que funcionan los
     cambios a mitad de línea (SCX, BGP, LCDC, paletas CGB).
   - Las reglas de prioridad CGB/DMG, los atributos CGB, OPRI y el bloqueo de VRAM/OAM/paletas
     que ya existen se conservan, ahora a nivel de punto.
   - Temporización exacta al punto de STAT: los bordes de modo, LY=LYC (incluido el
     comportamiento en la línea 153), el bloqueo de interrupciones STAT y la primera línea
     después de encender el LCD.
2. **OAM DMA real:** dura 160 M-ciclos. Mientras corre, la CPU solo puede leer HRAM (fuera
   de HRAM ve el byte que copia la DMA), y se puede reiniciar.
3. **Timer:** el retraso de recarga de TIMA (un M-ciclo con TIMA = 0 antes de cargar TMA y
   pedir la interrupción), y las escrituras a TIMA y TMA durante ese ciclo.
4. **CPU:** el orden exacto de los accesos a memoria y de los ciclos internos dentro de las
   instrucciones (CALL, RET, RETI, JP, RST, PUSH, `ADD SP,e`, `LD HL,SP+e`), y el caso de
   `ie_push`, que cancela la interrupción cuando el push sobrescribe IE.

## Decisiones del asistente

- **Reemplazar, no convivir:** un solo PPU. Mantener dos duplica el código y las pruebas.
  El cambio entra cuando el FIFO pasa todo lo que hoy pasa.
- **Alcance:** las fallas de Mooneye que son de temporización (PPU, OAM DMA, timer, CPU).
  Quedan fuera MBC2 y los multicartuchos MBC1, que no son de precisión.

## No objetivos

- MBC2, MBC1 multicart, Super Game Boy, cámara/impresora.
- Los valores exactos de DIV y de los registros de I/O al arrancar el CGB (`boot_div-cgb*`,
  `boot_hwio-C`). Para eso hace falta hardware real; se queda como está.
- El modo de bajo consumo de STOP en DMG.
- Tocar el core de GBA.

## Anti-objetivos (fallar aunque "funcione")

- Que deje de pasar algo que hoy pasa: `cgb-acid2`, `dmg-acid2` (DMG y CGB), Blargg
  `cpu_instrs` e `instr_timing` (DMG y CGB), Mooneye `boot_regs-*`, `hblank_ly_scx_timing-GS`,
  los tests unitarios de GB y GBA, y las 54 ROMs de Mooneye que hoy pasan en cada modelo.
- Que el emulador no llegue a 59.7 FPS en las PCs del usuario.
- Dos renderizadores en paralelo.

## Rendimiento

El usuario tiene otras cosas corriendo en su PC y pidió que se le recuerde el consumo. Hay
que medir el tiempo de `ezy_gb_runner` en Release para 3600 frames de `cgb-acid2` y de una ROM
de Mooneye, **antes** del cambio (en la rama base) y **después**, y reportar ambas cifras y la
proporción del tiempo real (60 s de juego). La barrera dura es la velocidad real (59.7 FPS)
en esta PC. Menos del 25 % de un núcleo es una meta blanda: si no se cumple, se reporta y se
propone una optimización, pero no bloquea (decisión del asistente). Se empieza con un bucle
simple por punto y se optimiza solo si la medición lo pide.

## Verificación

- **La meta es imitar el hardware, no forzar que pase una ROM** (decisión del asistente). Las
  ROMs de Mooneye sin sufijo de modelo se verificaron en hardware DMG/MGB/SGB, y en CGB real
  algunas fallan. Por eso las obligatorias de abajo son **obligatorias con `--model dmg`**.
  Las que tienen sufijo `-C` son obligatorias con `--model cgb`. En CGB, las demás se corren
  y se reportan, pero no bloquean. Sí se modela lo que cambia en CGB cuando hay una
  diferencia documentada; un PPU con ramas por modelo, no dos PPU.
- **Paso 0 (decisión del asistente):** antes de tocar código, en la rama base, guardar en
  `roms/README.md` la lista exacta de las ROMs de Mooneye que pasan hoy en cada modelo, junto
  con la medición de rendimiento. Esa lista es la barrera contra regresiones.
- **Obligatorias:**
  - `acceptance/ppu/`: `intr_2_0_timing`, `intr_2_mode0_timing`,
    `intr_2_mode0_timing_sprites`, `intr_2_mode3_timing`, `intr_2_oam_ok_timing`,
    `stat_irq_blocking`, `stat_lyc_onoff`, `vblank_stat_intr-GS` (DMG), `lcdon_timing-GS` (DMG),
    `lcdon_write_timing-GS` (DMG), `intr_1_2_timing-GS` (DMG), `hblank_ly_scx_timing-GS` (DMG).
  - `acceptance/oam_dma/*`, `oam_dma_start`, `oam_dma_restart`, `oam_dma_timing`.
  - `acceptance/timer/*`.
  - `call_timing`, `call_timing2`, `call_cc_timing`, `call_cc_timing2`, `ret_timing`,
    `ret_cc_timing`, `reti_timing`, `jp_timing`, `jp_cc_timing`, `rst_timing`, `push_timing`,
    `add_sp_e_timing`, `ld_hl_sp_e_timing`, `interrupts/ie_push`.
  - `misc/ppu/vblank_stat_intr-C` (CGB).
- **Deseables (no bloquean):** el resto de Mooneye `acceptance/` que aplique a cada modelo.
  Hay que reportarlas.
- Añadir a ctest las obligatorias que ahora pasen, y actualizar la lista de fallos conocidos
  en `roms/README.md`.
- Tests unitarios nuevos para el fetcher/FIFO (descarte por SCX, ventana, pausa por sprite,
  cambio de BGP a mitad de línea), OAM DMA (duración, bloqueo del bus, reinicio) y la recarga
  de TIMA.

## Restricciones

- Mismas que el proyecto de GBC: presets de CMake en `build/default`, código, comentarios y
  commits en inglés, y el estilo de `src/core/gb`.
- Los save states deben incluir el estado nuevo del PPU, la DMA y el timer. Como el formato
  cambia, se sube `kStateVersion`, y los estados viejos se **rechazan**, no se migran (decisión
  del asistente; mismo criterio que ya existe).
- Doble velocidad: el PPU sigue avanzando 2 puntos por M-ciclo de CPU en doble velocidad.
  La OAM DMA y el timer van al reloj de la CPU, como hoy. Las peculiaridades exclusivas del
  CGB (OAM DMA y HDMA a la vez, rarezas del OAM scan del CGB) quedan **fuera**: se conserva el
  comportamiento actual (decisión del asistente).

## Lo que queda al builder

- La estructura interna del fetcher y de los FIFOs, y el orden de los commits. Sugerencia:
  primero timer, CPU y DMA, que son cambios pequeños, y luego el PPU.
- Si hace falta un "tick por punto" en el PPU o basta con avanzar por M-ciclo con un bucle
  interno.
- Las fuentes de referencia: Pan Docs, *The Cycle-Accurate Game Boy Docs*, y los fuentes
  `.s` de Mooneye que vienen junto a cada ROM.
