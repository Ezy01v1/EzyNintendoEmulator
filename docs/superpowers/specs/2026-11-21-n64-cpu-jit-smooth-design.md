# JIT de la CPU, Fase 4: sin tirones (2026-11-21)

## Para qué

El usuario jugó MK64 con `ezyemu --gfx-hle --cpu-jit`: se ve bien y va a velocidad completa
(~63 campos/s en la carrera, por encima de los 60 NTSC), pero notó **un tirón pequeño en
DK's Jungle Parkway llegando al puente**. Lo que quiere de esta fase, en sus palabras: "que no
haya tirones". La meta deja de ser la media de campos/s y pasa a ser **que ningún campo tarde de
más**, también la primera vez que el juego entra en una zona.

Sigue valiendo la meta de fondo del proyecto (`2026-11-18-cpu-jit-core-design.md`): el mismo
núcleo `ezy_jit` servirá a GameCube/Wii, que tienen mucho más código y necesitan compilar rápido.

## Lo que se midió (2026-11-21, i7-4790, MK64, `--audio-hle --rdp-threaded --gfx-hle --cpu-jit`)

- En los 400 campos de la carrera (1500–1900) el JIT **sigue compilando**: los bloques
  compilados (total acumulado) pasan de 11 742 a 35 145 y el tiempo de compilación de 2,5 s a 6,5 s, es decir ~4 s en 400
  campos (**~10 ms por campo de media**, el presupuesto entero de un campo a 60 Hz es 16,7 ms).
- El perfil de la carrera (`docs/performance/2026-11-18-n64-cpu-jit.md`) da 47 % del hilo
  principal a AsmJit; ~30 % es su asignador de registros (`x86::Compiler`), ~10 % emisión.
- Con un contador temporal (ya retirado): de ~35 000 compilaciones, **~22 000 (≈ 2/3) empiezan
  dentro de un bloque que ya existe** (dirección entre su inicio y su final). Causa, en
  `Vr4300Jit::execute`: cuando un bloque no cabe antes del próximo evento, el intérprete avanza
  instrucción a instrucción hasta el evento, y la siguiente entrada al JIT cae en un PC
  arbitrario, donde `execute` compila un bloque nuevo (casi copia de la cola de otro).
  El contador también incluye destinos de salto legítimos dentro de otro bloque (la cabeza de
  un bucle), así que 2/3 es un techo; la medición del paso 2 da la cifra real.
- Sospecha (sin confirmar): el tirón del puente es una ráfaga de compilación al entrar código
  nuevo. El usuario confirmará luego si se repite en la segunda vuelta (si no se repite, casi
  seguro es compilación). El diseño no depende de esa respuesta.

## Metas

1. **Ningún campo de la carrera por encima de 16,7 ms** en el runner (campo más lento), en la
   misma máquina, con el JIT compilando "en frío" (desde el arranque, sin caché previa).
2. **Tiempo de compilación en la carrera 1500–1900 al menos un 75 % menor** (de ~4 s a ≤ 1 s).
3. **La media no empeora**: campos/s de la carrera ≥ la línea base en pares intercalados
   (mediana, al menos 4 de 5 pares sin pérdida mayor del 2 %).
4. **Mismo resultado exacto que el intérprete** (como siempre): estado, imágenes y pruebas.

(Umbrales decididos por el agente a partir de lo medido; el usuario puede corregirlos.)

Hoy un campo medio de la carrera dura ~15,9 ms (63 campos/s) y ~10 ms son compilación, así que
la meta 1 es alcanzable si la meta 2 se cumple. Un campo que pase de 16,7 ms con un tiempo de
compilación propio pequeño (menos de 2 ms) no cuenta contra la meta 1: se documenta como pico
ajeno a la CPU (RDP, audio) y queda fuera de esta fase.

## No metas

- Compilar en un hilo aparte (enfoque B). Si tras esta fase quedan campos > 16,7 ms, se
  plantea como una ronda nueva con los números en la mano.
- Guardar bloques en disco entre sesiones (enfoque C, descartado: no ayuda la primera vez).
- FPU nativa, enlace directo de bloques, registros entre bloques: dan media, no quitan
  tirones; quedan para una ronda posterior.
- Los tirones que no vengan de la CPU (RDP, audio, ritmo de cuadros de ezyemu): si la medición
  los señala, se anotan, no se arreglan aquí.
- La entrada PowerPC.

## Anti-metas

- Cualquier divergencia con el intérprete.
- Ganar media a costa de picos peores.
- Un asignador de registros o un backend atado a MIPS: vive en `ezy_jit`, sin saber de N64.
- Activar el JIT por defecto en ezyemu en esta fase (lo decide el usuario jugando, después).

## Restricciones

- C++20, MSVC Build Tools 2022, Windows x64, solo presets de CMake.
- AsmJit sigue siendo la dependencia (mismo SHA); se deja de usar `x86::Compiler` y se emite
  con `x86::Assembler`.
- Formato de estados sin cambios; el JIT no guarda nada.
- Responder en español; código, commits y PRs en inglés.
- Rama nueva `n64/jit-smooth` desde `n64/vi-output` (PR #18), como las anteriores, apilada.

## Enfoques considerados

| | Qué es | A favor | En contra |
|---|---|---|---|
| **A. Compilar menos y más barato en el mismo hilo (elegido)** | No compilar en PCs arbitrarios + backend con `Assembler` y asignador propio | Sin hilos ni sincronización nueva; ataca las dos causas medidas; GameCube necesitará compilar rápido igual | Una ráfaga muy grande de código nuevo aún cuesta algo en el hilo principal |
| B. Compilar en otro hilo | El intérprete sigue mientras un hilo compila | Quita casi todos los picos | Mucho más complejo (RAM que cambia durante la compilación, invalidación entre hilos); no quita el desperdicio de compilar de más |
| C. Caché en disco | Guardar bloques entre sesiones | Arranques más rápidos | No ayuda la primera vez en una zona; el código depende de direcciones de cada sesión |

El usuario eligió **A** (2026-11-21), con la medición primero.

## El diseño

### 1. Medir primero (runner)

- `--benchmark` imprime, además de lo de hoy, **por campo**: tiempo de pared, y de él el máximo,
  el percentil 99 y cuántos campos pasan de 16,7 ms; y el tiempo de compilación del JIT por
  campo (máximo y total en el tramo medido). Con `--frame-times <csv>` guarda una fila por campo
  (campo, ms, ms de compilación) para ver dónde están los picos.
- Opción `--hold <botones>[@campo]` en el runner: mantiene pulsados botones del mando 1 (por
  ejemplo `A` = acelerar) desde ese campo, para que una carrera cargada con `--load-state` avance
  sola. Los nombres de botones, los del frontend. Sin la opción, nada cambia (sin entrada).
- Escenarios de medición:
  - La carrera de siempre (1500–1900, la demo de MK64), en frío.
  - Cuando el usuario guarde con F2 un estado en DK's Jungle Parkway antes del puente
    (`E:\Rom nintendo\Mario Kart 64 (USA).ss1`), ese estado con `--hold A`. El estado del usuario
    no entra al repo.
- Antes de tocar nada se mide la línea base con esto y se documenta, y se guarda una copia del
  runner Release de la línea base (fuera del repo, en `build/`) para las parejas intercaladas.

### 2. No compilar en PCs arbitrarios

Regla: cuando `execute` no encuentra bloque para el PC, **el PC cae dentro de un bloque ya
compilado** (después de su primera palabra y hasta su última) **y no se llegó a él por un salto**
(la instrucción anterior no fue la ranura de retardo de un salto: `in_delay_slot_` falso), no
compila: sale y el intérprete da ese paso. Un destino de salto se compila siempre, aunque esté
dentro de otro bloque: es la cabeza de un bucle o una función, y debe correr nativo. Lo que la
regla evita son las reanudaciones arbitrarias (tras un evento que no dejó caber un bloque, o el
regreso de una interrupción con `ERET`). El intérprete sigue paso a paso hasta salir de ese bloque (su final
es un salto con su ranura, o el límite de instrucciones), y ahí el JIT encuentra o compila el
bloque siguiente, que es un inicio "natural".

- Para que la comprobación sea barata: una marca por palabra de "cubierta por un bloque" en las
  páginas con código (o el bloque que la cubre), mantenida al insertar y borrada al invalidar.
  El formato exacto lo elige el constructor; debe costar O(1) por consulta. La marca no
  distingue la clave de modo (decisión del agente): la clave casi nunca cambia en un juego y,
  si cambia, lo peor es interpretar unas instrucciones de más, nunca un resultado distinto.
- Un PC que nunca fue cubierto (código nuevo, destino de salto, tras una instrucción fuera de
  bloque) se compila como hoy.
- No cambia el resultado: qué ejecuta el JIT y qué el intérprete nunca cambia el estado (meta 4
  del proyecto original), solo cuánto cuesta.
- Se mide sola, antes del paso 3: compilaciones totales, compilaciones en la carrera,
  instrucciones extra que da el intérprete por la regla, campos/s y picos.

### 3. Backend con `x86::Assembler` y asignador propio

- `ezy_jit` gana un **asignador de registros lineal** sobre el IR, sin AsmJit: una pasada hacia
  atrás calcula el último uso de cada valor; una pasada hacia delante asigna registros del host
  de un conjunto fijo y, cuando no quedan, desaloja al valor cuyo próximo uso está más lejos a una
  ranura de la pila del bloque. Es genérico (tipos del IR, no registros MIPS) para que la entrada
  PowerPC lo reutilice.
- Llamadas a ayudas (`Call`, la ruta lenta de `NoteRamWrite`): los valores vivos tras la llamada
  van en registros que la llamada conserva (en Windows x64: rbx, rbp, rsi, rdi, r12–r15) o se
  guardan alrededor de ella; la llamada respeta el ABI (espacio reservado de 32 bytes, pila
  alineada a 16).
- `ctx`, la base de los registros invitados y la base de la RAM quedan fijas en registros
  conservados durante todo el bloque, cargadas en el prólogo.
- Prólogo y epílogo guardan solo los registros conservados que el bloque usa.
- Las salidas (`exit_ops`) se emiten detrás del cuerpo como hoy; el estado del asignador en el
  punto del salto es el que ven sus operaciones.
- El backend actual con `Compiler` se conserva **solo mientras se valida** el nuevo, detrás de un
  interruptor interno de pruebas (las pruebas del núcleo corren el mismo IR por los dos y comparan
  el resultado); al terminar la fase se borra.
- Se mide: tiempo de compilación por bloque (medio y máximo), tamaño de código, picos.

### Orden y puertas

1. Medición (paso 1) y línea base documentada.
2. Regla de entrada (paso 2), medida. Aunque baste para las metas 1–2, se sigue con el paso 3:
   el usuario eligió las dos partes y GameCube necesitará el compilador barato.
3. Backend nuevo (paso 3), medido.
4. Si al final no se cumplen las metas 1–2: se documenta el perfil que queda y se propone la
   ronda siguiente (B u otra), sin seguir añadiendo optimizaciones en esta fase.

## Validación

- Todo lo que ya existe con `--cpu-jit`: pruebas diferenciales intérprete/JIT, PeterLemon y
  n64-systemtest en Release (183), `--cpu-jit-check`, MK64 `--dump-state 1900` idéntico al del
  intérprete con RDP síncrono y con hilos, con y sin HLE gráfico; Spider-Man igual.
- Regla de entrada: pruebas de que un PC dentro de un bloque no compila y el de un bloque nuevo
  sí; de que invalidar una página borra sus marcas; y el estado de MK64 idéntico.
- Asignador y backend: pruebas del núcleo con IR escrito a mano que fuerzan desalojos (más
  valores vivos que registros), valores vivos a través de llamadas, salidas con valores
  desalojados, enteros de 32 y 64 bits; cada una por los dos backends con el mismo resultado.
  Una prueba de "muchos bloques aleatorios" (IR generado) comparando ambos backends.
- Velocidad: pares intercalados (al menos 5) contra la línea base del paso 1: campos/s, campo
  más lento, p99, campos > 16,7 ms y compilación en la carrera. Documento nuevo en
  `docs/performance/`.
- Al final, el usuario juega MK64 (DK's Jungle Parkway incluida) y dice si notó tirones.

## Lo que queda al constructor

- Formato de la marca "palabra cubierta" y cómo se borra al invalidar.
- Detalles del asignador: conjunto exacto de registros, cuándo reutilizar el registro de un
  operando para el resultado, cómo se resuelven las operaciones con registros fijos de x86
  (desplazamientos por `cl`, `mul`/`imul` de 128 bits con `rdx:rax`).
- Tamaño y alineación del marco de pila del bloque.
- Cómo se lee el mando en el runner para `--hold` (debe usar la misma ruta que ezyemu).
- Si el percentil 99 se calcula exacto (ordenando) o aproximado.
