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

## Backend nuevo (Assembler con asignador lineal)

`x86::Assembler` con un asignador de registros lineal propio (`reg_alloc.{hpp,cpp}`: 7 registros asignables,
expulsa el valor cuyo próximo uso está más lejos, huecos de pila por valor, copias de salida en los saltos)
en lugar de `x86::Compiler`. El emisor del Compiler y el enum `Backend` se borraron en este mismo cambio:
`compile(const IrBlock&, CodeBuffer&)` tiene la firma original y un solo emisor (`x64_backend.cpp`).

Mismo comando y ventana (Release, MK64, campos 1500-1900), con la regla de entrada incluida. 5 pares
intercalados (impares base→nuevo, pares nuevo→base); `base` es `build/baseline/ezy_n64_runner.exe`.

| par | runner | campos/s | max ms | p99 ms | media ms | campos > 16,7 ms | compile_max ms | compile_total ms |
|---|---|---|---|---|---|---|---|---|
| 1 | base | 64,27 | 368,1 | 111,9 | 15,53 | 133 | 351,2 | 3495 |
| 1 | nuevo | 117,91 | 94,1 | 22,2 | 8,45 | 34 | 78,8 | 309 |
| 2 | nuevo | 115,99 | 85,8 | 25,1 | 8,56 | 40 | 71,1 | 302 |
| 2 | base | 61,67 | 367,8 | 119,3 | 16,18 | 166 | 351,5 | 3526 |
| 3 | base | 63,87 | 378,9 | 114,7 | 15,63 | 143 | 361,1 | 3495 |
| 3 | nuevo | 118,26 | 85,2 | 21,4 | 8,42 | 20 | 70,8 | 303 |
| 4 | nuevo | 118,54 | 87,7 | 21,1 | 8,40 | 28 | 71,5 | 301 |
| 4 | base | 63,94 | 376,1 | 112,5 | 15,61 | 146 | 359,0 | 3489 |
| 5 | base | 63,14 | 374,1 | 113,6 | 15,81 | 153 | 356,6 | 3474 |
| 5 | nuevo | 112,46 | 88,6 | 30,2 | 8,86 | 53 | 72,9 | 304 |

Mediana: **63,87 → 117,91 campos/s** (los 5 pares ganan entre 1,78x y 1,88x). Compile total de la
carrera (mediana): **3495 ms → 303 ms (-91,3 %)**.

`CPU JIT:` nuevo (idéntico en los 5 pares): blocks 13445 (igual que antes del backend nuevo: el
backend no cambia qué se compila), in place 29,73 M, native 293,1 M, out of block 1,33 M, exits before
563 541, invalidations 208, flushes 0, resume skips 2 700 742, **compile ms ~480 en total desde el arranque**
(base: 35145 bloques, ~5740-5820 ms).

Tiempo de compilación por bloque (compile ms total / bloques compilados, ambos de la línea `CPU JIT:`):

| runner | bloques | compile ms | ms por bloque |
|---|---|---|---|
| base | 35145 | ~5786 | 0,165 |
| nuevo | 13445 | ~479 | 0,036 |

(4,6x más barato por bloque; el resto de la mejora viene de compilar 2,6x menos bloques, Tarea 2.)

### Perfil (`--profile build/vo/p4.csv`, un recorrido)

Una ejecución con `--profile` del runner nuevo (120,8 campos/s con el perfilador). Las filas `asmjit::*`
suman **2,95 %** de las muestras (antes, con el Compiler, la compilación llegaba al 47 % del hilo principal);
con el asignador propio y el emisor (`reg_alloc.cpp`, `x64_backend.cpp`) el total de la compilación es
3,5 %. Lo más alto ahora es "RSP native code" 14,6 %, `ZwYieldExecution` 10,9 % (espera del hilo
principal al RDP), `Vr4300Jit::execute` 10,6 %, `execute_cop1` 4,1 % (y `fesetround`, `fdclass`, `fdtest` y
`fegetenv`, ~10 % entre las cuatro; la FPU sigue en el intérprete).

### Veredicto contra los objetivos

1. **Ningún campo de carrera sobre 16,7 ms: NO cumplido.** En los 5 pares nuevos quedan 20-53 campos
   > 16,7 ms (max 85-94 ms; base 133-166, max ~370 ms). De ellos, los que tienen compile_ms >= 2 ms (cuentan):
   8, 4, 5, 5 y 5 por ejecución; el resto (26, 36, 15, 23, 48) tienen compile_ms < 2 ms, picos que no son
   de CPU/JIT (documentados como tales; máximo 22 ms, ruido de ejecución y del hilo del RDP). En la base
   los 133-166 campos lentos tenían todos compile_ms >= 2 ms. Los campos con compilación son casi siempre
   los mismos: **1510-1512** (carga inicial de código de la carrera: 45 + 10 + 78 ms de compilación, el
   peor 85-94 ms), 1549 (~16 ms de compilación, 31 ms el campo), y 1713, 1759, 1771, 1853, 1891
   (compilaciones de 2-4 ms que empujan un campo a 17-22 ms). Solo 2 campos superan 33 ms (antes 42-43).
2. **Compilación de la carrera (1500-1900) al menos 75 % menor (~4 s a <= 1 s): cumplido.** 3495 ms →
   303 ms de mediana (-91,3 %; las 5 parejas entre 301 y 309 ms).
3. **Campos/s medio no menor (mediana de 5 pares, >= 4 de 5 sin perder más de 2 %): cumplido.**
   Mediana 63,87 → 117,91 campos/s, 5 de 5 pares ganan (+78 % a +88 %).

### Estado de DK's Jungle Parkway (puente)

**Pendiente**: el usuario todavía no guardó un estado ahí (el `Mario Kart 64 (USA).ss1` existente es del
2026-11-19, anterior a esta fase, y no es ese punto). Cuando lo guarde (F2 en ezyemu), medir con
`--load-state "E:/Rom nintendo/Mario Kart 64 (USA).ss1" --hold A` durante 600 campos, antes (runner de
`build/baseline/`) y después: campo máximo y campos > 16,7 ms.

## Cierre de la fase y lo que queda

Objetivos 2 y 3 cumplidos con mucho margen; el objetivo 1 no. Lo que queda de los campos lentos de CPU son
~5 campos de compilación por ejecución, casi todos en **1510-1512**: la carga inicial de código de la
carrera, que compila ~135 ms de bloques (45 + 10 + 78 ms) en el hilo principal, y un campo (1549) con ~16 ms. No se añadió ninguna optimización en esta tarea.

Propuesta para la ronda siguiente (sin implementar):

- **Compilar en segundo plano**: el hilo principal sigue con el intérprete mientras otro hilo compila el
  bloque y lo publica (el bloque se enlaza en la siguiente entrada). Quita por completo los picos de 1510-1512
  y 1549, que son los que quedan; necesita publicar la caché de bloques de forma segura (puntero atómico por
  bloque) y cuidado con las invalidaciones (208 en la ventana).
- **Alternativa más pequeña**: compilar con presupuesto por campo (p. ej. máx 2 ms de compilación por
  campo; el resto de las entradas nuevas siguen en el intérprete y se compilan en campos siguientes). Reparte los
  ~130 ms en varios campos sin hilos, a costa de ejecutar más tiempo en el intérprete justo tras la carga.
- Los ~15-48 campos por ejecución que pasan de 16,7 ms con compile_ms < 2 ms (16,7-22 ms) no vienen del JIT:
  mirar el hilo del RDP (`ZwYieldExecution` 10,9 %) y el RSP en una ronda aparte.
- FPU nativa (`execute_cop1` ~4 % más ~10 % de `fesetround`/`fdclass`/`fdtest`/`fegetenv` en el perfil).

Comprobación tras borrar el Compiler (Release recompilado, mismo código de emisión): MK64 en 1900 campos
con `--dump-state` da SHA-1 4a4c0711b75a574a8f50bb2b49934bb6a494d518 (igual). Las ejecuciones sueltas de
después (88-108 campos/s) salieron con la máquina más cargada (tras 4 minutos de ctest); en una pareja
base→nuevo→base→nuevo inmediata: base 58,7 y 59,2; nuevo 110,4 y 112,3 (1,9x), sin regresión.
