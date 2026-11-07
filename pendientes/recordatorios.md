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

1. [ ] **Optimizar rendimiento GB**: Pokémon Silver usa 32 % de un núcleo (25 % antes del FIFO).
       Idea: saltar el bucle por dot en modos 0/1/2; los cambios de LY en el dot 453 y el dot 1 de la línea 153 deben seguir exactos.
2. [ ] **ROMs Mooneye que fallan**: `ppu/hblank_ly_scx_timing-GS` (regresión del pase de precisión),
       `lcdon_timing-GS`, `lcdon_write_timing-GS`, `misc/ppu/vblank_stat_intr-C`. Detalles en `roms/README.md`.
3. [ ] **Probar Pokémon Silver a fondo**: guardado en el juego, RTC día/noche, glitches gráficos.
4. [ ] **GBA: probar juegos comerciales**. Hay que copiar dumps `.gba` propios a `roms/` (ignorado por git).
       Pokémon Emerald sirve para probar el RTC. Usar `ezy_gba_runner --screenshot`.
5. [ ] (Opcional) La ventana VRAM del debugger se encima con la ventana Screen (posición fija).

## Roadmap

GBA (terminar pendientes) → **N64** → Nintendo DS → GameCube → Wii.
Wii U / Switch solo después, si vale la pena. **No** NES ni SNES.

## Cómo trabajar

- Uso varios PCs: el código se mueve solo por git (`git pull` al empezar, `git push` al terminar).
- Compilar solo con presets: `cmake --preset default` → `build/default`,
  `cmake --build --preset debug`, `ctest --preset debug`.
  Si CMake se queja de rutas, borrar `build/default` y reconfigurar.
- Runners para probar ROMs: `ezy_gb_runner --screenshot`, `ezy_gba_runner --screenshot`.
- Las ROMs van en `roms/` y no se suben al repo.

## Bitácora

- 2026-11-05 — GBA terminado, PR #1. Se agregaron CMake presets.
- 2026-11-06 — GBC mergeado (PR #2). Pase de precisión GB mergeado (PR #3). Se crea esta carpeta.
