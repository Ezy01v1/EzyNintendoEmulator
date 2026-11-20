# N64: salida del VI con sus filtros (2026-11-19)

## Para qué

El usuario jugó MK64 en ezyemu (`--gfx-hle --cpu-jit`) y lo encontró fluido, pero "no se mira
de lo mejor". Tras corregir el Z del RDP (commit 73bc414), la diferencia que queda contra ares
v148 en el mismo instante de la demo es la del VI: ezyemu muestra el framebuffer crudo, con
bordes en escalera, tramado visible y una fila de basura abajo; la consola real (y ares) pasa
la imagen por el antialias, el filtro de tramado, el divot, el reescalado con interpolación y
la gamma, y solo muestra la ventana visible. Meta: que ezyemu se vea como la consola.

## Cómo es hoy

- `Vi::image()` (src/core/n64/vi.cpp) lee el framebuffer de `VI_ORIGIN` a su propia resolución
  (ancho de `H_VIDEO` × `X_SCALE`, alto nominal 240/288 × `Y_SCALE`), RGBA5551 o RGBA8888 a
  ARGB, sin ningún filtro. Altura nominal en vez de `V_VIDEO`: por eso aparece la última fila.
- El frontend (`N64System::render`, src/frontend/n64_system.cpp) lo estira por vecino más
  cercano a un lienzo fijo de 640×480; SDL lo escala a la ventana también por vecino.
- El runner (`--screenshot`, `--compare`, `--series`, `--benchmark`, `--dump-state` no) y las
  91 pruebas PeterLemon de CTest comparan `image()` contra BMP de referencia del framebuffer.

## Metas

1. Una salida nueva del VI para la pantalla, fiel al hardware, con estas etapas según
   `VI_CTRL`/`VI_STATUS`:
   - **Lectura** del framebuffer de 16 o 32 bits con su cobertura (16 bits: bit 0 del píxel y
     los 2 bits ocultos de RDRAM; 32 bits: bits 7:5 del byte de alfa). La cobertura solo se usa
     con `AA_MODE` 0 o 1; con 2 o 3 cuenta como completa.
   - **Antialias** (`AA_MODE` 0/1) en píxeles con cobertura parcial: mezcla con el mínimo y el
     máximo por canal de los vecinos con cobertura completa, ponderado por cuánta cobertura
     falta.
   - **Filtro de tramado** (bit 16) en píxeles con cobertura completa: corrige cada canal con
     sus ocho vecinos para recuperar la precisión perdida por el tramado de 16 bits.
   - **Divot** (bit 4): en bordes (algún píxel del trío sin cobertura completa), mediana por
     canal del píxel y sus vecinos izquierdo y derecho.
   - **Reescalado** horizontal y vertical con `X_SCALE`/`Y_SCALE` (2.10, desplazamiento en los
     bits altos) e interpolación bilineal con fracciones de 5 bits; `AA_MODE` 3 replica sin
     interpolar.
   - **Gamma** (bit 3) y **tramado de gamma** (bit 2).
   - **Ventana visible**: solo `H_VIDEO`/`V_VIDEO` (con los desplazamientos de NTSC y PAL); fuera
     de ella, negro. Entrelazado (bit 6) por campos.
2. ezyemu muestra esa salida, a 640×480 (NTSC) o el tamaño que dé la ventana del VI en PAL,
   con escalado lineal a la ventana del sistema operativo. Activada por defecto, con
   `--vi-raw` para ver el framebuffer crudo como hoy.
3. Comparada con ares en el mismo instante de la demo de MK64 (la herramienta de captura y
   emparejamiento ya existe en esta sesión), la diferencia visible debe desaparecer: bordes
   suaves, sin tramado, sin fila de basura.

## No metas

- Cambiar `Vi::image()`: sigue siendo el framebuffer crudo para las pruebas de hardware y las
  herramientas.
- Mover el VI a la GPU (shaders): queda para cuando exista el RDP en GPU.
- Filtros extra que la consola no tiene (CRT, escalado "bonito").

## Anti-metas

- Romper cualquiera de las 183 pruebas Release (PeterLemon, con y sin JIT, y systemtest) o las
  suites unitarias.
- Copiar código de angrylion-rdp-plus: su licencia (MAME) prohíbe el uso comercial y obliga a
  sus condiciones en obras derivadas. Se implementa a partir del comportamiento documentado
  (n64brew "Video Interface") y se usa angrylion y ares solo para comparar resultados.
- Que la salida filtrada baje la velocidad de juego de forma notable: si cuesta más de ~2 ms
  por cuadro en Release, se mueve a un hilo antes de activarla por defecto.

## Restricciones

- C++20, MSVC 2022, presets de CMake. Código, comentarios y commits en inglés.
- Sin cambiar el formato de estados (el VI no gana estado guardado nuevo; el ruido de la gamma
  con tramado usa un generador propio no guardado, o se deja determinista).
- El estado emulado no cambia: la salida es solo una lectura de RDRAM y registros.

## Enfoques considerados

| | Qué es | A favor | En contra |
|---|---|---|---|
| **A. VI en C++, CPU (elegido)** | `Vi::output()` aplica las etapas sobre RDRAM | Exacto, probable píxel a píxel, sin infraestructura nueva | Coste de CPU por cuadro (a medir) |
| B. Shader de suavizado en GPU | Filtro bilineal/blur al mostrar | Barato | No usa la cobertura: suaviza todo, no se parece a la consola |
| C. VI exacto en GPU | Compute shader | Más rápido | Requiere el pipeline de GPU que aún no existe |

Elegido A por el agente (el usuario delegó el cómo): es lo único que da el resultado de la
consola hoy, y su lógica se podrá portar a un shader cuando llegue C.

## Diseño

### Prerrequisito: cobertura de 16 bits en el RDP

En RGBA5551 la cobertura de 3 bits se guarda así en el hardware: el bit alto en el bit 0 del
píxel y los dos bajos en los bits ocultos de RDRAM de ese halfword (el noveno bit de cada byte).
Hoy el RDP de ezyemu solo escribe el bit 0 (rdp.cpp:~952) y al leer supone los bajos
encendidos (rdp.cpp:~747 y ~862); la tabla `RdpHiddenBits` ya existe pero solo la usan los
códigos DZ del Z-buffer. Sin los bits bajos el antialias del VI casi no actúa en MK64 (16 bits).

Decisión del agente: el RDP escribe también los dos bits bajos de la cobertura en los bits
ocultos al escribir color de 16 bits, y `memory_coverage` los lee de ahí, como el hardware. Los
rellenos (fill) copian el bit bajo de cada halfword a sus bits ocultos (3 o 0), como el
hardware; hoy los borran, lo que dejaría el fondo con cobertura parcial y el VI lo suavizaría.
En el antialias, `penmin`/`penmax` son el segundo menor y el segundo mayor del conjunto
(decisión del agente: la implementación de angrylion tiene una asimetría que parece un efecto
de su optimización; se valida contra ares). El formato de estados no cambia (la
tabla ya se guarda desde la versión 11; solo cambia su contenido). Es la primera tarea, con
sus propias pruebas, y debe mantener las 183 pruebas Release (PeterLemon compara contra
hardware real).

### El filtro

- `src/core/n64/vi_filter.hpp/.cpp`: funciones puras sobre una vista de RDRAM (más los bits
  ocultos) y una copia de los registros del VI: lectura con cobertura, antialias, tramado,
  divot, reescalado, gamma. Cada etapa probada por separado con framebuffers sintéticos.
- `Vi::Image Vi::output(std::span<const std::uint8_t> rdram, <lector de bits ocultos>) const`:
  arma la imagen final. Los bits ocultos viven en `RdpRasterizer::hidden_` (privado, y `Rdp`
  hereda en privado): hace falta un accesor de solo lectura en `Rdp` que el `Bus` pase al VI.
- **Tamaño de salida** (decisión del agente): siempre el lienzo completo del televisor, 640
  de ancho por 480 líneas (NTSC) o 576 (PAL); lo que queda fuera de `H_VIDEO`/`V_VIDEO`, en
  negro. Sin entrelazado, cada línea del campo se dobla; con entrelazado (bit 6) se tejen los
  dos campos, conservando el anterior para las líneas del otro campo (como la persistencia del
  televisor).
- Frontend: el lienzo de N64 sigue siendo de 640×480; una salida PAL de 640×576 se reduce a
  ese lienzo conservando la proporción (sin textura de tamaño variable en este proyecto). El
  escalado de SDL sigue en vecino más cercano (la suavidad la da el VI, como en la consola).
- `N64::output()` junto a `N64::image()`; el frontend usa `output()` salvo con `--vi-raw`.
- Runner: `--vi-output` hace que `--screenshot`/`--series` guarden la salida filtrada en vez
  del framebuffer (para comparar con ares).
- Con el RDP en hilos, `output()` drena el RDP como ya hace `Bus::image()` (bus.hpp:~94): se
  llama una vez por cuadro.

## Validación

- Pruebas unitarias por etapa con valores calculados a mano desde la descripción del hardware
  (un borde con cobertura 3 entre colores conocidos, un patrón de tramado 2×2, un píxel suelto
  para el divot, un reescalado 320→640 con fracciones, la tabla de gamma).
- Las 11 suites y las 183 pruebas Release sin cambios.
- MK64: capturas filtradas (`--vi-output`) frente a ares en los instantes ya emparejados
  (ares en ventana, recortando su barra de estado y escalando a 640×480). La diferencia media
  por píxel debe ser menor que la del framebuffer crudo en todos los pares, y a ojo sin
  escalera ni tramado ni fila de basura. Comprobar también Spider-Man. La aceptación final es
  la del usuario jugando en ezyemu.
- Coste: tiempo de `output()` por cuadro en Release (objetivo < 2 ms); si no, hilo.
- El usuario lo mira en ezyemu.

## Lo que queda al constructor

- Estructura interna de las etapas y nombres, mientras cada una se pruebe sola.
- El orden exacto de fetch/divot/reescalado alrededor de los bordes de línea (seguir la
  documentación y comparar contra ares).
- Si el generador de ruido de la gamma con tramado es determinista o propio.
