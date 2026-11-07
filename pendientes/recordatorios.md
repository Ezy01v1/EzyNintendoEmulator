# Recordatorios / contexto entre sesiones

Este archivo es la memoria compartida del proyecto entre PCs y cuentas de Claude.
**Claude: léelo al empezar cada sesión y actualízalo al terminar** (qué se hizo, qué quedó pendiente).
Responder siempre en español; código, commits y PRs en inglés.

_Última actualización: 2026-11-06_

## Estado actual

- **GBA**: core terminado (keypad IRQ, RTC, open bus, prefetch, save states). PR #1 a `main`.
- **GB/GBC**: mergeado en `main` (PR #2). Pasa cgb-acid2, dmg-acid2, Blargg cpu_instrs/instr_timing, Mooneye boot_regs-cgb.
- **Precisión GB**: PPU pixel-FIFO, OAM DMA, retraso de TIMA, timing de interrupciones (PR #3, mergeado). Mooneye 54 → 80 por modelo.
- Specs y planes de diseño: `docs/superpowers/specs` y `docs/superpowers/plans`.
- `roms/pokemon-silver.gbc` arranca y llega a la intro de Oak.

## Pendientes (en este orden, antes de empezar N64)

1. [x] **Optimizar rendimiento GB**: hecho (PR #4, mergeado). El PPU salta los dots sin eventos
       de los modos 0/1/2. Pokémon Silver: 24 % → 12 % de un núcleo (medido en la PC con Build Tools).
2. [ ] **ROMs Mooneye que fallan**: `misc/ppu/vblank_stat_intr-C` ya pasa (PR #4).
       Quedan `ppu/hblank_ly_scx_timing-GS`, `lcdon_timing-GS` y `lcdon_write_timing-GS`: hay que
       recalibrar juntos el origen de línea del PPU, el dummy fetch y la salida de HALT.
       Lo que se probó y las cuentas están en `roms/README.md`.
3. [ ] **Probar Pokémon Silver a fondo**: guardado en el juego, RTC día/noche, glitches gráficos.
       La intro se ve bien en `ezy_gb_runner --screenshot`; falta jugarlo en `ezyemu` (necesita al usuario).
4. [ ] **GBA: probar juegos comerciales**. Hay que copiar dumps `.gba` propios a `roms/` (ignorado por git).
       Pokémon Emerald sirve para probar el RTC. Usar `ezy_gba_runner --screenshot`.
5. [x] (Opcional) La ventana VRAM del debugger se encima con la ventana Screen (posición fija).
       Ya resuelto en `f82824b`: VRAM va en y=410, debajo de Screen (y=10..400).

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
