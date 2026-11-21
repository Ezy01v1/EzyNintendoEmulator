# Transferencias vectoriales nativas del RSP

## Selección medida

Contadores temporales en `RspVector::execute` y `access_memory`, con Mario Kart 64
USA, 300 campos, Release y RDP síncrono. Los histogramas del intérprete y JIT son
idénticos byte por byte y ambas imágenes coinciden con la referencia. Los
contadores se retiran antes de medir velocidad.

| Operación | Ejecuciones |
| --- | ---: |
| VMADN | 6.713.927 |
| VMADH | 5.313.818 |
| VMADM | 3.589.466 |
| SSV | 4.107.879 |
| SDV | 3.714.696 |
| LSV | 3.275.055 |
| LDV | 2.558.313 |
| LLV | 1.865.492 |
| SLV | 544.790 |

Las seis transferencias simples de 2/4/8 bytes suman 16.066.225 instrucciones.
El perfil anterior atribuía ~3,9 % de muestras propias a `access_memory` y ~3,8 %
al thunk vectorial compartido. Estas transferencias permiten un alcance pequeño
sin cambiar multiplicaciones, acumuladores, saturaciones ni banderas. Los datos
locales están en `build/vector-frequency-{interpreter,jit}.log` y
`build/vector-frequency-summary.json`; no se atribuye coste en segundos a cada
opcode a partir solo de su frecuencia.

## Implementación propuesta

Generar directamente LBV/LSV/LLV/LDV y SBV/SSV/SLV/SDV, cuyos tamaños son 1/2/4/8
bytes. El opcode y elemento ya son constantes al compilar el bloque. Dirección
base y contenido de registros siguen leyéndose del estado actual en ejecución.

La vista estándar del JIT añade un puntero opcional a las 32 filas de ocho
lanes `uint16_t` del RSP. El padre `RspRegs` entrega esa dirección mediante acceso
de amistad, sin exponer un nuevo acceso mutable público a los registros. No se
usan offsets del layout privado de `RspVector` ni se serializan punteros del host.

En x64, el byte vectorial big-endian `e` ocupa el desplazamiento `e ^ 1` en cada
fila de 16 bytes almacenada como lanes host-endian. Cada acceso DMEM usa máscara
4095; stores envuelven el elemento con máscara 15, loads truncan en el final del
registro. El offset de 7 bits se extiende con signo y se escala por el ancho.
No cambian el presupuesto por instrucción, PC, fase ni orden de eventos.

Para elementos pares sin envoltura del registro se agrupan accesos en chunks de
4 bytes y, si quedan, 2 bytes. Una comparación sin signo exige que todo el tramo
quepa en DMEM; los cruces y elementos impares conservan la ruta por bytes. Se
intercambian bytes dentro de cada lane con `ROR16 8`, o `BSWAP32` seguido de
`ROR32 16`. No se requieren extensiones SIMD ni accesos alineados de x86.

Una vista sin puntero nativo mantiene el helper anterior; si tampoco tiene
helper, sale sin retirar instrucciones. Las claves históricas no incorporan
direcciones: la misma traducción lee la vista de la llamada actual.

## Verificación y decisión

Primero una prueba directa sin helper debe fallar con el emisor anterior.
Después probar 4096 combinaciones de carga/escritura, ancho, elemento, offset y
base, incluyendo ambos extremos de memoria y registros. Comparar todos los bytes
y la salida temporal, además del respaldo al helper sin vista nativa.

Ejecutar las siete suites, equivalencia de imágenes y estados de MK64 y medición
alternada contra una base sin instrumentación, ambos ejecutables con LTCG completo.
Conservar el cambio solo si los resultados apoyan la mejora; mantener el backend
intérprete y RDP síncrono predeterminados. No hay merge a `main` en este alcance.

## Primera medición y refinamiento

La primera implementación emitía cada acceso por byte. Pasó las siete suites:
192 casos / 36344 comprobaciones por combinación N64; sin JIT, 179 / 1973 en
ambos modos RDP. Imágenes MK64 a 120/300/600 campos y continuación entre backends
300+120 en ambas direcciones coincidentes, con cero vaciados de caché. La nueva
prueba sin helper falló primero (0 instrucciones retiradas en vez de 1) y pasó
después; cubre 4096 combinaciones. Otras pruebas usan el helper real y cambian
el puntero de registros con la traducción ya en caché, incluido el registro 31.

Tres pares fríos, base/candidato/candidato/base/base/candidato, ambos con LTCG
completo y sin compilaciones propias simultáneas:

| Variante | Tiempo real (s) | CPU (s) |
| --- | --- | --- |
| Base | 9,947 / 10,004 / 9,997 | 9,625 / 9,734 / 9,688 |
| Nativa por bytes | 10,541 / 9,689 / 11,100 | 9,844 / 9,562 / 10,703 |

Medianas: base 9,997 s reales / 9,688 s CPU, candidato 10,541 / 9,844.
No demostró mejora; no se conserva como resultado de rendimiento. Los contadores
de instrucciones, compilaciones y reutilizaciones fueron idénticos, y todas las
imágenes exactas. Datos: `build/vector-native-jit-bytewise.json`.

Se refina el mismo grupo con los accesos empaquetados descritos arriba: el emisor
por bytes produce varias operaciones de dirección por cada byte. El agrupamiento
reduce esas operaciones cuando el tramo completo cabe; se repiten las mismas
pruebas y la comparación del juego completo. Revisión técnica independiente de
ambas rutas sin hallazgos; la medición del refinamiento se documenta a continuación.

## Resultado de los accesos agrupados

La ruta agrupada mantiene las siete suites en verde (192 casos / 36344
comprobaciones por combinación N64), las imágenes exactas a 120/300/600 campos
y la igualdad de estados completos e imágenes en ambas direcciones tras
300+120 campos. ON restaurado; runner y frontend Release recompilados. La
compilación OFF ya pasó sus dos suites, 179 casos / 1973 comprobaciones cada una;
el refinamiento posterior solo cambia el código condicionado a JIT.

Se midieron dos lotes de seis procesos fríos, en órdenes complementarios. Base y
candidato son ambos **JIT**, con RDP síncrono, mismo preset, LTCG completo y sin
instrumentación ni compilaciones propias simultáneas. No es una comparación
contra el intérprete. Las doce imágenes fueron exactas, con 1316 compilaciones,
190375 reutilizaciones y ningún vaciado en todos los procesos.

| Lote | Variante | Tiempo real (s) | CPU (s) |
| --- | --- | --- | --- |
| Base/candidato/candidato/base/base/candidato | Base | 52,256 / 35,587 / 30,271 | 19,062 / 14,688 / 17,156 |
| Igual | Agrupada | 40,336 / 19,807 / 21,173 | 16,625 / 14,062 / 13,719 |
| Candidato/base/base/candidato/candidato/base | Base | 21,475 / 17,030 / 22,975 | 15,422 / 13,641 / 14,922 |
| Igual | Agrupada | 31,044 / 16,019 / 16,557 | 15,531 / 12,391 / 13,672 |

La variante agrupada mejora CPU en **5/6 parejas**. Mediana de reducción por
pareja: **8,77 % de CPU** y **25,37 % de tiempo real**. Medianas agrupadas:
15,172 / 13,891 s CPU y 26,623 / 20,490 s reales (base/candidato).

La variación del tiempo real es muy alta, también en la base: no se presenta
el 25 % como una aceleración general ni se comparan estos segundos con lotes
anteriores como si las condiciones fueran iguales. El resultado de CPU y su
dirección en cinco parejas apoyan conservar esta optimización dentro del JIT
experimental. No demuestra tiempo real, una mejora frente al intérprete ni la
condición completa para activarlo por defecto. La ROM histórica de systemtest
sigue ausente y su baseline 2984/3721 no se atribuye a esta entrega.

Datos y reproducción locales: `build/vector-native-jit.json`,
`build/vector-native-jit-reverse.json`, `build/vector-native-analysis.json`,
`build/benchmark-vector-native.ps1` (con y sin `-Reverse`) y
`build/validate-vector-native.ps1`. No se retienen los contadores temporales en
el core. Próximo grupo a evaluar: VMADN/VMADH/VMADM, con 15.617.211 ejecuciones
combinadas en el histograma, midiendo su coste antes de ampliar la emisión.
