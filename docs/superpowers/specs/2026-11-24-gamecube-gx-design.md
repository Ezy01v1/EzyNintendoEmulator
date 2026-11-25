# GameCube, subproyecto 2: GX en la GPU con Direct3D 11 (2026-11-24)

## Para qué

Segundo subproyecto de la hoja de ruta de GameCube (`2026-11-21-gamecube-base-design.md`). El
subproyecto 1 dejó a Mario Kart: Double Dash!! (MKDD) en su bucle principal: cada campo envía unas
300 primitivas al FIFO gráfico, pero el emulador solo las cuenta y la pantalla queda negra. El
objetivo del usuario: **ver la pantalla de título de MKDD**. Este subproyecto dibuja lo que el juego
manda (el procesador gráfico "GX" de la consola: transformación, rasterizado, texturas y el
combinador TEV) en la tarjeta de video del PC.

Decisiones del usuario (2026-11-24): **Direct3D 11** como API gráfica y **la copia del EFB al XFB
vuelve a la RAM** (como en la consola). El resto son decisiones del agente (el usuario delegó las
decisiones de GameCube el 2026-11-21), marcadas como tales.

## Metas

1. **La pantalla de título de MKDD se ve** en el runner (`--screenshot`) y en ezyemu. El usuario
   confirma que se reconoce (logo, fondo, texto), comparando con su recuerdo o con capturas
   públicas del juego. Criterio medible del runner: un campo después del arranque cuyo XFB no es
   negro ni de un solo color, con el registro de progreso describiendo qué se ve.
2. **Pruebas exactas del GX** con WARP (el rasterizador por software de Direct3D, idéntico en
   cualquier PC con Windows 10/11): para cada pieza (vértices, transformación, rasterizado, cada
   formato de textura, etapas TEV, profundidad, mezcla, prueba de alfa, copia del EFB) una prueba
   con un comando GX pequeño y los píxeles esperados.
3. **Nada se ignora en silencio** (regla global del proyecto): un registro BP/XF/CP, un formato
   de textura o una función del TEV que no se implemente detiene la máquina con un error que
   nombra el registro o el valor. Así MKDD dice qué le falta.
4. Las otras consolas no cambian y las suites existentes siguen pasando.

## No metas

- Velocidad: dibujar en la GPU no acelera la CPU interpretada (~4 campos/s). Eso es el
  subproyecto 4 (JIT).
- Mejoras visuales (resolución interna mayor, filtros, pantalla ancha).
- Audio (subproyecto 3), tarjeta de memoria, Wii, PAL/480p.
- Optimizar la copia del EFB dejándola solo en la GPU ("XFB en textura"): queda para cuando haya JIT.
- Otras APIs (OpenGL, Vulkan): la interfaz del backend queda separada para poder añadirlas, pero no
  se escriben.

## Anti-metas

- Trucos por juego (detectar MKDD, saltarse efectos).
- Un renderizador que solo funcione bien en la tarjeta del PC de desarrollo: las pruebas corren en
  WARP; el hardware real es el modo normal de ezyemu.
- Romper o ralentizar N64, GB o GBA.
- Que el núcleo (`ezy_gc`) dependa de Direct3D: el backend vive aparte.

## Restricciones

- C++20, MSVC Build Tools 2022, Windows 10/11 x64, solo presets de CMake, `/W4` limpio.
- Sin dependencias nuevas: Direct3D 11, DXGI y D3DCompiler vienen con el Windows SDK de la PC
  (10.0.26100) y con Windows (`d3dcompiler_47.dll`, `d3d10warp.dll`, comprobado).
- Responder en español; código, commits y PRs en inglés. Rama `gc/gx` desde `main`.

## Enfoques considerados

| | Qué es | A favor | En contra |
|---|---|---|---|
| **Direct3D 11 (elegido por el usuario)** | Backend D3D11, sombreadores HLSL generados y compilados con D3DCompile | WARP da imágenes idénticas en cualquier PC: pruebas exactas y runner sin ventana; todo viene con Windows | Solo Windows (el proyecto ya lo es) |
| OpenGL 4.5 | GLSL compilado por el driver | Multiplataforma | Píxeles distintos por fabricante: pruebas frágiles; contexto sin ventana más torpe |
| SDL_GPU | La API nueva de SDL sobre D3D12/Vulkan | Moderna | Los sombreadores del TEV se generan en tiempo de ejecución y SDL_GPU necesita una herramienta extra para compilarlos |

| Copia EFB → XFB | Qué es | Decisión |
|---|---|---|
| **Vuelve a la RAM (elegido por el usuario)** | Leer el EFB de la GPU y escribirlo en RAM en el formato de la consola | Exacto; el VI, el runner y ezyemu siguen mostrando el XFB desde la RAM como hoy |
| Solo en la GPU | El VI muestra una textura | Más rápido; queda como optimización futura |

**Consecuencia importante (decisión del agente):** como el cuadro vuelve a la RAM, **el frontend no
cambia**. El dispositivo Direct3D del GX es propio del núcleo GameCube, sin ventana; ezyemu sigue
recibiendo píxeles ARGB por `System::render` como todas las consolas, e ImGui sigue con el
renderizador de SDL. Esto evita tocar el camino de N64/GB/GBA.

## El diseño

### Piezas

- `src/core/gc/gx/` (parte de `ezy_gc`, sin Direct3D):
  - **Estado GX**: los registros CP (formatos de vértice VCD/VAT, bases y pasos de los arrays
    indexados), XF (matrices de posición/normal/textura, proyección, viewport, canales de color e
    iluminación, generación de coordenadas de textura) y BP (TEV, texturas, profundidad, mezcla,
    prueba de alfa, niebla, tijera, culling, copia del EFB). Hoy `gx_fifo` solo analiza tamaños;
    pasa a entregar cada comando a este estado.
  - **Cargador de vértices**: decodifica cada vértice (directo o indexado desde RAM, todos los
    formatos y escalas de la consola) a un formato canónico en float.
  - **Transformación en la CPU** (decisión del agente): posición, normales, iluminación (canales
    de color) y generación de coordenadas de textura se calculan en la CPU con la aritmética del
    XF, y la GPU recibe vértices ya en espacio de recorte. Más sencillo de probar con exactitud y
    portable a cualquier backend; el coste es pequeño con 300 primitivas por campo. Si el perfil lo
    pide, se mueve a la GPU más adelante.
  - **Decodificador de texturas**: todos los formatos (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8,
    CMPR, C4/C8/C14X2 con paletas TLUT) a RGBA8 en la CPU, con caché por dirección, formato,
    tamaño y contenido (hash), invalidada cuando el contenido cambia.
  - **Configuración del TEV y del pixel** a una **clave** (etapas, entradas, operaciones,
    escalas, sesgo, swap tables, konst, prueba de alfa, niebla, formato del EFB): el backend genera
    un sombreador por clave y lo guarda en caché.
  - **Interfaz `GxBackend`** (abstracta): crear/actualizar texturas, dibujar un lote de triángulos
    con un estado, borrar, copiar el EFB (a RAM, como XFB o como textura), leer píxeles.
- `src/core/gc/gx_d3d11/` (biblioteca aparte `ezy_gc_d3d11`, enlaza `d3d11`, `dxgi`,
  `d3dcompiler`):
  - Dispositivo D3D11 sin ventana: **hardware** por defecto, **WARP** con una opción (pruebas y
    `--gx-warp` en el runner).
  - EFB: objetivo de render 640×528 RGBA8 + profundidad 24 bits, emulando los formatos de pixel
    de la consola (RGB8_Z24, RGBA6_Z24, RGB565_Z16) cuantizando en el sombreador.
  - Sombreadores HLSL generados por clave (vértice simple + pixel con el TEV), compilados con
    D3DCompile y cacheados.
  - Copia del EFB: a una textura de staging, se lee y se convierte en la CPU: al XFB en YUV 4:2:2
    (con escala vertical, gamma y el borrado que ya existe), o a una textura en RAM en el formato
    pedido (los juegos usan copias a textura para efectos).
- `GameCube` recibe el backend al crearse (`GameCube::create(path, backend)`); el runner y ezyemu
  crean el D3D11; las pruebas del núcleo que no dibujan pueden usar un backend nulo.

### Orden de construcción

1. Estado GX y cargador de vértices completos (sin dibujar), con pruebas.
2. Backend D3D11 mínimo: dispositivo, EFB, triángulo con color de vértice, borrado, copia al XFB
   en RAM. Primera imagen de MKDD (aunque sea a colores planos).
3. Transformación e iluminación XF, culling, tijera, profundidad.
4. Texturas (todos los formatos y TLUT) y coordenadas de textura.
5. TEV completo, prueba de alfa, mezcla, niebla, formatos de pixel del EFB.
6. Copias del EFB a textura.
7. Arrancar MKDD hasta la pantalla de título, resolviendo cada parada (como la Tarea 7 del
   subproyecto 1), con registro de progreso.

### Las lecturas del EFB por la CPU

La CPU puede leer y escribir el EFB directamente (`0xC8000000`). Si MKDD lo hace, se implementa
sobre el backend (leer o escribir un pixel); si no, el acceso sigue siendo un error.

## Validación

- Pruebas unitarias en la CPU (exactas, sin GPU): cargador de vértices (cada formato, escala,
  indexado), transformación e iluminación, decodificador de cada formato de textura y TLUT,
  conversión YUV de la copia, generación de la clave del TEV.
- Pruebas de dibujo en WARP (exactas): un comando GX pequeño por función, comparando los píxeles
  del EFB/XFB con los esperados.
- MKDD: registro de progreso (`docs/progress/`) con lo que fue haciendo falta, capturas del runner
  en `build/gc/` (fuera del repo) y la confirmación del usuario en ezyemu.
- Las suites existentes siguen pasando; la suite gc en Debug sigue bien por debajo de su límite.

## Lo que queda al constructor

- La forma exacta de la clave del TEV y de las cachés.
- La agrupación de primitivas en lotes y el formato del búfer de vértices.
- El orden fino dentro de cada paso, y qué detalles del TEV implementar primero según lo que pida
  MKDD (siempre deteniéndose con error ante lo no implementado).
- Cómo se marcan las texturas sucias (hash completo o por rango de memoria escrito).
