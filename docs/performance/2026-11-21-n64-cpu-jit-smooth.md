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

## Regla de entrada

`Vr4300Jit::execute` ya no compila en un PC de reanudación arbitrario: si el PC cae dentro de un bloque
compilado (no en su primera palabra, `BlockCache::covered`) y no se llegó por un salto
(`in_delay_slot_` falso), se detiene y el intérprete sigue. Un destino de salto (cabeza de bucle) sigue
compilándose. El contador es `resume skips` en la línea `CPU JIT:` del runner.

Mismo comando y ventana que la línea base (Release, MK64, campos 1500-1900), 5 pares intercalados (se
alterna quién va primero: impares base→nuevo, pares nuevo→base). El SHA-1 del estado tras 1900 campos
sigue siendo 4a4c0711b75a574a8f50bb2b49934bb6a494d518.

| par | runner | campos/s | max ms | p99 ms | media ms | campos > 16,7 ms | compile_max ms | compile_total ms |
|---|---|---|---|---|---|---|---|---|
| 1 | base | 60,42 | 378,4 | 117,3 | 16,52 | 156 | 360,7 | 3652 |
| 1 | nuevo | 104,94 | 189,0 | 28,9 | 9,47 | 58 | 172,8 | 758 |
| 2 | nuevo | 105,24 | 192,8 | 27,6 | 9,47 | 66 | 177,8 | 769 |
| 2 | base | 55,29 | 398,2 | 122,5 | 18,02 | 175 | 380,6 | 3913 |
| 3 | base | 59,12 | 462,0 | 118,9 | 16,88 | 157 | 438,5 | 3771 |
| 3 | nuevo | 103,48 | 191,7 | 27,2 | 9,63 | 71 | 176,3 | 758 |
| 4 | nuevo | 106,73 | 183,8 | 25,3 | 9,31 | 59 | 168,1 | 730 |
| 4 | base | 61,29 | 384,7 | 123,1 | 16,27 | 163 | 369,5 | 3623 |
| 5 | base | 57,93 | 384,5 | 116,0 | 17,22 | 179 | 366,1 | 3693 |
| 5 | nuevo | 93,71 | 211,6 | 32,7 | 10,64 | 114 | 193,9 | 863 |

`CPU JIT:` nuevo (iguales salvo ruido): blocks 13445 (base 35145), in place 29,73 M, native 293,1 M,
out of block 1,33 M, exits before ~563,5 k (base ~1,18 M), compile ms ~1170-1350 en total desde el
arranque (base ~5900-6360), invalidations 208, flushes 0, **resume skips 2 700 742** (en los 1900 campos
de la ejecución completa con `--dump-state`: 12 671 239).

### Lectura

- Los bloques compilados bajan de 35 145 a 13 445 y el tiempo de compilación de la carrera (campos
  1500-1900) de ~3,6-3,9 s a ~0,73-0,86 s.
- Campos/s sube de 55-61 a 94-107 (mediana ~105 contra ~59). Campos > 16,7 ms: 156-179 -> 58-114;
  p99 ~120 ms -> ~27-33 ms; máximo ~380-460 ms -> ~184-212 ms.
- Lo que queda de compilación sigue dominando los peores campos (compile_max ~170-194 ms de ~184-212 ms):
  es la carga inicial de código, que esta regla no toca.
- Coste: tras una salida a mitad de bloque (presupuesto, evento, acceso MMIO) el resto del bloque lo ejecuta
  el intérprete hasta el siguiente salto; native baja un 0,7 % y los tests que contaban bloques entrados
  en ese punto se ajustaron (`exits before`, `native`, guardas del RDP).
