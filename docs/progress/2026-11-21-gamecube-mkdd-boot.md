# GameCube: arranque de MKDD (subproyecto 1, tarea 7)

Registro de lo que fue necesitando Mario Kart: Double Dash!! (GM4E01, volcado NKit v01 leído
directamente como GCM, regla R2) para llegar a su bucle principal con `ezy_gc_runner`, y la
corrida final del criterio (meta 1 del spec: 600 campos seguidos con datos en el FIFO gráfico
cada campo, interrupciones del VI y ninguna parada).

Comando: `build/default/tools/gc_runner/Release/ezy_gc_runner.exe "E:/Rom nintendo/Mario Kart
Double Dash/Mario Kart - Double Dash!! (USA).nkit.iso" --fields 600 [--screenshot ...]`.

Cada parada se arregla con comportamiento del hardware o del arranque HLE documentado (nada
por juego, nada por dirección), con su prueba unitaria.

## Paradas

### 1. `mtspr` SPR 952 (MMCR0) en `0x800A9654`, tras 132 511 instrucciones

El SDK escribe el monitor de rendimiento del 750 al iniciar. Se modelan los SPR 952-958
(MMCR0, PMC1, PMC2, SIA, MMCR1, PMC3, PMC4) como en el hardware y en Dolphin: se guardan y se
leen; las copias de usuario de solo lectura 936-942 (UMMCR0…UPMC4) leen los mismos registros
y no se pueden escribir. Ningún evento se cuenta (los PMC conservan lo escrito y nunca piden
la excepción del monitor). Prueba: `the performance monitor SPRs are stored, and their
user-level copies read them` (test_gekko_system.cpp).

### 2. EXI canal 0, dispositivo 2, en `0x800C3BD8`, tras 145 146 instrucciones

`EXIInit` del SDK lee `EXIGetID(0, 2)` buscando un depurador Barnacle en el puerto serie 1.
Dolphin pone ahí su dispositivo por defecto, None (`IEXIDevice`, que no transfiere nada). Se
modela igual: las lecturas dan 0 y las escrituras desaparecen, inmediatas o por DMA. El canal 2
(el AD16 de Dolphin) y las ranuras de tarjeta siguen deteniéndose. Prueba: `EXI channel 0
device 2 (serial port 1) is empty: writes vanish, reads give 0` (test_si_exi.cpp).

Con esto MKDD corre los 600 campos sin detenerse (VI 600 de 600), pero solo escribe al FIFO en
8 campos: hace 2 lecturas de disco y 10 correos al DSP (la subida del microcódigo), y luego
espera.

### 3. Espera sin fin en `0x800A1D20`: el DSP no saluda tras arrancar el microcódigo

El bucle lee el byte `-0x64C8(r13)` hasta que vale 1. Lo pone `__DSPHandler` del SDK (en
`0x800A2000`) al recibir, con la interrupción del DSP, el correo `DSP_INIT` 0xDCD10000 de la
primera tarea: es el protocolo de tareas del SDK (`DSPAddTask`), que todo microcódigo cumple
(Dolphin lo envía en el `Initialize` de cada uno).

El microcódigo de MKDD se identifica como lo hace Dolphin (`UCodeFactory`): el CRC
`HashEctor` de la imagen de IRAM que sube el SDK (0x00343680, 0x1D20 bytes) es **0x2FCDF1EC**,
el microcódigo "Zelda" de JAudio (Nintendo EAD; Dolphin `Zelda.cpp`, banderas
`MAKE_DOLBY_LOUD`: el protocolo por defecto). Se añade `dsp_zelda.hpp/.cpp` con ese protocolo
de correos. Arranque: `DSP_INIT` con interrupción y luego el saludo 0xF3551111. Los correos
DSP→CPU pueden llevar la interrupción del DSP como en el `CMailHandler` de Dolphin (sube cuando
ese correo pasa a ser el primero pendiente). Un microcódigo con otro CRC detiene la emulación
al arrancarlo, nombrando el CRC. Pruebas: `the DSP HLE identifies the microcode by its CRC and
stops on an unknown one`, `MKDD's microcode (Dolphin's Zelda) greets with DSP_INIT...`
(test_dsp_ai.cpp; la imagen de prueba se construye con ceros y unos bytes resueltos hacia
atrás para dar el CRC, sin copiar nada del microcódigo).

### 4. Correo 0x00000005 al microcódigo, en `0x800B3238`

Es el protocolo de comandos de Zelda: un correo `0x0000nnnn` anuncia un comando de n palabras
(aquí el 01, de configuración: voces por cuadro y direcciones de tablas). Se modela el
protocolo de `HandleMailDefault`/`RunPendingCommands`/`RenderAudio` de Dolphin correo por
correo, sin mezclar audio (subproyecto 3): cada comando se confirma con `DSP_SYNC` 0xDCD10004
(con interrupción) y 0xF3550000 | la mitad alta del comando; el 02 (renderizar n cuadros)
avanza cuadro a cuadro según los correos de sincronización de voces de la CPU (0x00000000
seguido de grupo << 16 | banderas), confirma cada cuadro con 0xF355FF00 | cuadro y termina con
`DSP_FRAME_END` 0xDCD10005 (con interrupción); los comandos que llegan mientras tanto esperan
hasta el 0xCDD10003 de la CPU. Los comandos 04-09 (que cuelgan el microcódigo real), los
inexistentes y los correos 0xCDD1000x distintos de 3 detienen la emulación. Los búferes de
salida de audio quedan como están. Pruebas: `MKDD's microcode acknowledges each command...` y
`MKDD's microcode renders frames as the CPU syncs the voices...` (test_dsp_ai.cpp).

### 5. Escritura de PIINT (bit 1 del control del DSP), en `0x800B3268`

`DSPAssertInt` del SDK, que JAudio llama tras el primer correo de cada comando: la CPU
interrumpe al DSP. El HLE la atiende al instante, como atiende los correos: no se guarda y se
lee 0 (como en el hardware una vez que el DSP la toma); no tiene otro efecto, porque el HLE no
la necesita para leer los correos. Prueba: en `DSP HLE answers the boot handshake mails`.

Con esto MKDD llega a su bucle principal: corre sin detenerse, con datos en el FIFO gráfico en
cada campo desde el 3 (unos 14-15 KB y ~298 primitivas por campo a partir del campo ~300),
lecturas de disco continuas y ~58 correos al DSP por campo.

### 6. (No es una parada) El XFB de MKDD mide 608 píxeles de ancho

La captura mostraba 32 píxeles de colores al final de la última línea. El volcado de estado
(`--dump-end`) dio la causa: la copia del EFB de MKDD es de 608×448 con paso de 1216 bytes, y el
VI lo dice en su configuración de imagen (WPL = 38 palabras de 32 bytes = 608 píxeles); el VI
leía siempre 640 píxeles por línea, así que cada línea tomaba 32 píxeles de la siguiente y la
última, 64 bytes de después del XFB. Ahora el ancho de la imagen es WPL × 16 píxeles (como el
`fbWidth` de Dolphin), hasta 640; ezyemu la escala a 640×480 y el runner la guarda tal cual.
Prueba: `the XFB picture is WPL * 16 pixels wide (MKDD's 608-pixel XFB)` (test_pi_vi.cpp).

La prueba de MKDD en `test_pi_vi.cpp` ahora exige que los 5 primeros campos corran sin parar y
que el DSP arranque el microcódigo 0x2FCDF1EC (unos 7 s en Debug; la suite cabe en su límite).

El protocolo del DSP no fue un bloqueo para el subproyecto 3: con respuestas al protocolo de
correos (sin mezclar) el juego avanza. Lo que queda para el subproyecto 3 es mezclar de verdad
(las voces de los VPB, los coeficientes AFC, la reverberación) y llenar los búferes de salida
que el AI reproduce.
