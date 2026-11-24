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
8 campos: hace 2 lecturas de disco y 10 correos al DSP, y luego espera algo. Siguiente paso:
averiguar qué espera.
