# N64: CPU JIT sin tirones (2026-11-21)

Rama `n64/jit-smooth`. Objetivo: que MK64 no tenga campos lentos por la compilación del JIT.

## Herramientas del runner

- `--benchmark ... --frame-times <csv>`: mide cada `run_frame()` cronometrado por separado y escribe
  `field,ms,compile_ms` (compile_ms = diferencia de `compile_ns` del JIT de CPU alrededor del campo; 0 sin JIT).
  Imprime `Frames: {max_ms, p99_ms (exacto), mean_ms, over_16_7, compile_max_ms, compile_total_ms}`
  sobre los campos cronometrados.
- `--hold NAME[+NAME...][@CAMPO]`: mantiene botones del mando 1 desde ese campo (por defecto 0), también
  durante el calentamiento. Nombres: A B Z Start Up Down Left Right L R CU CD CL CR. Sirve con
  `--screenshot`, `--series`, `--dump-state` y `--benchmark`. Sin `--hold` nada cambia (el SHA-1 del
  estado de MK64 tras 1900 campos sigue siendo 4a4c0711b75a574a8f50bb2b49934bb6a494d518).

## Línea base

Release, i7-4790, MK64 (sin `--hold`, carrera cargada por el propio arranque), campos 1500-1900
(400 campos), 3 ejecuciones:

```
ezy_n64_runner.exe "E:/Rom nintendo/Mario Kart 64 (USA).z64" --benchmark build/vo/b.bmp 1900 \
  --time-from 1500 --audio-hle --rdp-threaded --gfx-hle --cpu-jit --frame-times build/vo/base_frames.csv
```

| ejecución | campos/s | max ms | p99 ms | media ms | campos > 16,7 ms | compile_max ms | compile_total ms |
|---|---|---|---|---|---|---|---|
| 1 | 60,71 | 374,5 | 111,3 | 16,44 | 166 | 356,7 | 3595 |
| 2 | 61,80 | 373,9 | 110,4 | 16,15 | 160 | 357,7 | 3551 |
| 3 | 61,75 | 377,1 | 118,4 | 16,16 | 156 | 359,7 | 3585 |

`CPU JIT:` (las tres iguales salvo ruido): blocks 35145, in place 29,85 M, native 295,3 M, out of block
1,38 M, exits before ~1,18 M, compile ms ~5900 (en total desde el arranque), invalidations 208, flushes 0.

### Lectura

- Los campos lentos son casi todos de compilación: en las tres ejecuciones **todos** los campos > 16,7 ms
  tienen compile_ms > 1 ms. Los campos > 33 ms son 43-44 por ejecución, y el peor (~375 ms) compila ~357 ms.
- Dentro de la ventana se compilan ~3,55-3,6 s de los ~6,5 s totales: más de la mitad del tiempo es
  compilación, no ejecución. Sin ese tiempo solo 7-15 campos seguirían sobre 16,7 ms (máximo 18,6-27,6 ms
  restando la compilación), así que el resto del tirón es ruido de ejecución, no de compilar.
- 62-70 de los campos lentos caen en los campos 1700-1900 (la carrera en sí), no solo en la carga.
- La media de ~16,2 ms por campo (~61 campos/s) está justo en el límite de tiempo real de 59,94 Hz:
  el JIT en caliente tiene poco margen y la compilación lo rompe.
