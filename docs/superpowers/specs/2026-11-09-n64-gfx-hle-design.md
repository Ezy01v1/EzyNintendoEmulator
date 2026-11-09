# Diseño: Nintendo 64, HLE del microcódigo gráfico (F3DEX)

Fecha: 2026-11-09 · Rama prevista: `n64/gfx-hle` · Subproyecto 4 (velocidad), etapa 2

## Intención y por qué

Mario Kart 64 en carrera va a ~25 cuadros/s con el RDP en su hilo; hacen falta 60. El perfil
de la demo de carrera (cuadros 1500–1900 de `ezy_n64_runner`) reparte un cuadro así:

| Parte | Tiempo aprox. por cuadro | Dónde corre |
|---|---|---|
| RSP | ~17 ms | hilo principal |
| CPU VR4300 | ~13 ms | hilo principal |
| RDP | ~21 ms | su propio hilo |

Dentro del RSP, el **63 %** de las instrucciones son de tareas de gráficos (tipo 1) y el
**37 %** de audio (tipo 2). Afinar el intérprete del RSP ya no da más (probado: inline
forzado, predecodificación). La vía estándar es dejar de ejecutar el microcódigo gráfico
instrucción por instrucción y hacer en C++ lo mismo que hace: **HLE** (high-level emulation).

Estimación: sin el RSP gráfico, el hilo principal baja de ~31 ms a ~20 ms por cuadro y MK64
pasa de ~25 a **~40–45 cuadros/s**, limitado entonces por el RDP. Llegar a 60 necesitará
además HLE de audio y un RDP más rápido; ambos quedan **fuera** de este spec.

## Qué microcódigo

La ROM de MK64 incluye `RSP Gfx ucode F3DEX 0.95` y `RSP Gfx ucode F3DLX 0.95` (Yasumoto).
Son F3DEX de primera generación (GBI 1, con búfer de 32 vértices y `G_TRI2`). F3DLX es la
variante sin recorte en Z ("no near clip"). Ambos comparten casi todo; se implementan como
una sola HLE con dos banderas de comportamiento.

Fuera de alcance (posibles specs futuros): Fast3D (SM64), F3DEX2/F3DZEX (Zelda), S2DEX,
microcódigos propios de Rare/Factor 5, y audio.

## Objetivos

1. **Detección.** Cuando la CPU arranca el RSP (`SP_STATUS` quita `HALT`) y la tarea en
   `OSTask` (DMEM `0xFC0`) es de tipo 1, se identifica el microcódigo buscando su cadena de
   versión en `ucode_data` (RDRAM). Se reconoce F3DEX 0.95 y F3DLX 0.95. Cualquier otro
   microcódigo, o la HLE apagada, sigue por el RSP real como hoy.
2. **Intérprete de display lists** en C++ que recorre `data_ptr` con segmentos, pila de
   `G_DL` (profundidad 10) y `G_ENDDL`, e implementa los comandos de GBI 1 / F3DEX:
   - Estado: `G_MTX` (proyección y modelview, push/load/mul), `G_POPMTX`, `G_MOVEMEM`
     (viewport, luces, matriz), `G_MOVEWORD` (segmentos, número de luces, clip ratio,
     perspnorm, fog, `G_MW_POINTS` de F3DEX), `G_TEXTURE`, `G_SETGEOMETRYMODE` y
     `G_CLEARGEOMETRYMODE`, `G_SETOTHERMODE_H/L`, `G_RDPHALF_1/2`.
   - Vértices: `G_VTX` (búfer de 32), `G_MODIFYVTX` si F3DEX 0.95 lo tiene.
   - Primitivas: `G_TRI1`, `G_TRI2`, `G_QUAD` (si existe en 0.95), `G_CULLDL`,
     `G_BRANCH_Z`, `G_LOAD_UCODE` (cambio a otro microcódigo: si es reconocido sigue en HLE,
     si no se termina la tarea con el aviso correspondiente).
   - Paso directo al RDP: los comandos `0xC0–0xFF` que son comandos del RDP
     (`Set Other Modes`, `Load Block`, `Fill Rectangle`, `Texture Rectangle` con
     `G_RDPHALF`, `Sync Full`, etc.).
3. **Transformación de vértices con la aritmética del RSP**: matrices en punto fijo s15.16,
   productos y acumulación como `VMUDN/VMADH`, iluminación direccional + ambiente con
   normales s8, `G_TEXTURE_GEN` y lineal, niebla, banderas de recorte, y transformación de
   viewport con el recíproco de W tal como lo calcula `VRCP/VRCPH` (reutilizar
   `reciprocal()` de `rsp_vector.cpp`). La meta es **el mismo resultado entero** que el
   microcódigo, no solo uno parecido.
4. **Preparación de triángulos para el RDP**: con tres vértices de pantalla se generan los
   comandos de triángulo `0x08–0x0F` (bordes, shade, textura, Z) con los mismos
   coeficientes que calcula F3DEX. Incluye culling por cara (`G_CULL_FRONT/BACK`).
5. **Recorte** contra los planos del clip ratio (guard band) y el plano cercano (F3DEX;
   F3DLX no recorta en Z), en el mismo orden y con la misma interpolación que el
   microcódigo, para que los triángulos recortados coincidan.
6. **Salida al RDP sin pasar por RDRAM**: los comandos generados se entregan al RDP por una
   entrada interna (`Rdp::submit_hle`) que comparte el camino de dibujo (y el hilo del RDP).
   `DPC_START/END/CURRENT/STATUS` se actualizan como si el RSP hubiera enviado la lista por
   XBUS/RDRAM, para que lo que lea la CPU sea coherente.
7. **Fin de tarea y tiempos**: al terminar la lista, el RSP queda en `HALT | BROKE`, se
   escriben las señales (`SIG2` "task done") y se levanta la interrupción de SP como haría
   el `BREAK` del microcódigo. Ocurre tras un tiempo estimado proporcional al trabajo
   (vértices y triángulos procesados), agendado en el planificador, no al instante, para no
   cambiar el orden de eventos que los juegos esperan. `Sync Full` sigue levantando la
   interrupción de DP desde el RDP.
8. **Interruptor.** `N64::set_hle_gfx(bool)`; apagado por defecto (pruebas y runners siguen
   exactos), encendido en `ezyemu`, y `--hle-gfx` en `ezy_n64_runner`.
9. **Save states**: la HLE ejecuta cada tarea completa de una vez, así que entre tareas no
   guarda estado propio salvo el de fin de tarea agendado. Se guarda ese evento y la
   identificación del microcódigo; versión de estado 12.

## Validación

La pieza central es **comparar contra el RSP real**, que ya es correcto:

1. **Traza de comandos del RDP.** Un modo de depuración del runner graba cada palabra que el
   RDP consume (`--rdp-trace <archivo>`). Se graba la demo de MK64 con el RSP real y con la
   HLE, y una herramienta compara ambas trazas comando por comando, mostrando el primer
   comando distinto y sus campos decodificados. Esto localiza errores de redondeo en
   vértices, bordes o recorte sin adivinar a partir de la imagen.
2. **Imagen.** La captura de la demo (`--screenshot ... 1900`) con HLE se compara con la del
   RSP real. Meta: idéntica; aceptable si la traza solo difiere en coeficientes con error de
   1 ulp documentado.
3. **Pruebas unitarias** de las piezas puras: multiplicación de matrices en punto fijo,
   transformación de un vértice, luces, recíproco, coeficientes de un triángulo conocido y
   recorte de un triángulo que cruza el plano cercano, cada una contra valores obtenidos con
   el RSP real ejecutando el microcódigo.
4. Las 154 pruebas actuales y n64-systemtest (2984/3721) sin cambios: con la HLE apagada no
   cambia nada, y n64-systemtest no usa F3DEX.
5. **Velocidad:** `--time-from 1500` en la demo, A/B seguidas, con y sin `--hle-gfx`.

## Etapas (cada una cierra con traza comparada y commit)

1. **Detección y esqueleto.** Identificación del microcódigo, recorrido de la display list,
   segmentos, `G_DL/ENDDL`, paso directo de comandos del RDP, fin de tarea con interrupción.
   Herramienta de traza del RDP. Validación: las listas que solo tienen comandos del RDP
   (rellenos, rectángulos del HUD) coinciden.
2. **Matrices y vértices.** `G_MTX`, `G_POPMTX`, `G_MOVEMEM/MOVEWORD`, `G_VTX` con
   transformación, viewport, niebla y luces. Validación: volcar el búfer de vértices de la
   HLE y del RSP real (DMEM) en el mismo punto y compararlos.
3. **Triángulos.** `G_TRI1/TRI2`, culling y coeficientes del RDP. Validación: traza.
4. **Recorte** (y `G_CULLDL`, `G_BRANCH_Z`). Validación: traza en escenas con triángulos que
   cruzan el borde de la pantalla y la cámara.
5. **Tiempos, save states, interruptor en ezyemu y medición final.**

## Riesgos

- **Coincidencia exacta de los coeficientes.** El microcódigo calcula los bordes y
  pendientes con instrucciones vectoriales y recíprocos de precisión limitada; replicarlo
  exige seguir su secuencia de operaciones. Las fuentes públicas (desensamblados y HLE de
  otros emuladores) describen F3DEX de forma general y suelen apuntar a la GPU, no a
  comandos del RDP. Mitigación: la traza comparada encuentra cada diferencia en el comando
  exacto, y el RSP real está siempre disponible como referencia.
- **Diferencias entre 0.95 y versiones posteriores** de F3DEX (opcodes y orden de
  operaciones). Mitigación: implementar contra lo que hace el microcódigo de la ROM,
  verificando con la traza; documentar cada diferencia hallada.
- **Juegos que leen DMEM o dependen del tiempo exacto del RSP gráfico.** Mitigación: la HLE
  es opcional y el modo exacto sigue disponible.
- **Ganancia menor a la estimada** si el RDP pasa a ser el límite antes. Se mide en la etapa
  5; la siguiente optimización (RDP o audio HLE) se decide con esos números.

## Fuera de alcance

HLE de audio, otros microcódigos gráficos, RDP en GPU, JIT. Cada uno, si se aborda, tendrá
su propio spec.
