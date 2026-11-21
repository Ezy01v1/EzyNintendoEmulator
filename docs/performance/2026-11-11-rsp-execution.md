# Perfil de ejecución del RSP tras reutilizar traducciones

## Objetivo y método

Continuación autorizada después de publicar `codex/n64-rsp-jit` hasta `e7cc68f`.
Identificar el coste restante de ejecución antes de ampliar el JIT. Se mantiene
el intérprete y el RDP síncrono como opciones predeterminadas.

Se usa el runner Release, Mario Kart 64 USA y los primeros 300 campos desde
arranque en frío. Cada proceso compara exactamente la imagen final con
`build/mk64-sync-300.bmp`. No se mide una carrera con entrada del jugador.

El muestreador local `build/profile-rsp-execution.ps1` suspende brevemente el hilo
principal, lee RIP y lo reanuda en un `finally`. DbgHelp resuelve el PDB. Las
direcciones de páginas privadas ejecutables se clasifican como código nativo
del RSP mientras el proceso sigue vivo, antes de que sus páginas se liberen.
Los porcentajes son muestras propias por función, no tiempo inclusivo de pila.

Se obtuvieron 1595 muestras para el intérprete y 1783 para el JIT. Sin resolver:
2,0 % y 2,5 %, respectivamente. El perfil perturba el tiempo de ejecución:
sus tiempos no se usan como benchmark. Los datos están en
`build/rsp-execution-interpreter.json` y `build/rsp-execution-jit.json`.

## Resultado del perfil

| Función o región | Intérprete | JIT |
| --- | ---: | ---: |
| `RspRegs::execute_scalar` | 11,4 % | 4,4 % |
| `RspRegs::run` | 7,8 % | 4,8 % |
| Código nativo RSP | 0 % | 6,3 % |
| `RspJit::run` | 0 % | 3,3 % |
| `RspRegs::jit_vector` | 0 % | 3,8 % |
| `RspVector::execute_alu` | 5,6 % | 5,0 % |
| `RspVector::access_memory` | 4,4 % | 3,9 % |
| `N64::step` | 7,3 % | 5,8 % |

El JIT reduce la interpretación escalar, pero introduce entradas, marcos nativos
y llamadas a helpers. Las funciones vectoriales y el RDP siguen ejecutándose
en C++. La compilación ya está acotada: 1316 bloques y 190375 reutilizaciones
por 300 campos, sin vaciados. No se justifica un JIT de CPU ni otra caché a partir
de estos datos. La ejecución está repartida entre varias áreas.

Agrupando funciones con RIP exclusivo, RSP representa 41,6 % de las muestras
del intérprete y 41,9 % del JIT (incluido su código nativo). RDP representa
28,3 % / 25,4 %; CPU VR4300/COP0/COP1/TLB 10,1 % / 9,5 %; compilación AsmJit
0 % / 3,4 %. El resto incluye control de N64, bus y planificador. Estos grupos
describen dónde investigar; no implican que eliminar un componente conserve el
resto de tiempos ni que los porcentajes comparen segundos absolutos entre modos.

## Experimento acotado: omitir entradas que terminarán en fallback

Hipótesis: el bucle intenta entrar al JIT incluso para saltos, COP0, BREAK y
opcodes no soportados. Evitar esas llamadas puede reducir parte del 3,3 % de
`RspJit::run`, a cambio de leer y clasificar el primer opcode antes de entrar.

Se mueve la clasificación existente a una función compartida por el compilador
y el bucle; no se amplía el conjunto soportado. Se conserva la invalidación por
generación antes de cualquier ejecución nativa, los delay slots, single-step,
presupuestos, eventos empatados y fallback. No cambia el formato de estados.

El candidato pasa las siete suites Debug, incluidas las cuatro combinaciones
RSP/RDP. Para aislar la medición se recompilan tanto base como candidato con
LTCG completo, retirando únicamente los artefactos conocidos del runner dentro
de `build/`. No se cambia ninguna opción de compilación. Se conservan los dos
ejecutables para medirlos sin compilar simultáneamente.

Se harán tres procesos fríos de cada variante, en orden base/candidato/candidato/
base/base/candidato, con JIT y RDP síncrono. Todos comparan la imagen exacta de
300 campos. Se registran tiempo real, CPU, compilación e instrucciones nativas.
Una diferencia pequeña o contradictoria con CPU no cuenta como mejora demostrada;
si el cambio no ayuda se retira y se conserva el diagnóstico.

## Resultado del experimento: retirado

Las imágenes fueron exactas en las doce ejecuciones. Las dos variantes ejecutan
76.828.052 instrucciones nativas, 31.073.143 interpretadas y compilan 1316 bloques.
La revisión independiente no detectó problemas de comportamiento ni de invalidación.

El primer lote dio mediana real 20,289 s base / 14,384 s candidato, y CPU
13,438 / 12,203 s. Dada la variación, se repitió con el orden complementario
candidato/base/base/candidato/candidato/base. Ese lote dio 18,398 / 23,975 s
reales, y 13,641 / 11,875 s de CPU. No hubo compilaciones propias simultáneas.

| Pareja | Orden | Cambio de tiempo real | Cambio de CPU |
| --- | --- | ---: | ---: |
| 1 | Base / candidato | −0,24 % | +4,88 % |
| 2 | Candidato / base | −31,46 % | −9,19 % |
| 3 | Base / candidato | −10,32 % | −1,12 % |
| 4 | Candidato / base | −30,17 % | −19,93 % |
| 5 | Base / candidato | +69,13 % | +7,38 % |
| 6 | Candidato / base | +89,17 % | +3,97 % |

Un signo negativo significa menos tiempo con el candidato. Gana CPU solo en
3/6 parejas; la mediana de sus cambios emparejados es **+1,42 % de CPU**.
La mediana real emparejada baja un 5,28 %, con cambios desde −31 % hasta +89 %.
Las medianas agrupadas de las doce ejecuciones parecen mejores (19,343 / 16,290 s
reales; 13,539 / 12,039 s CPU), pero ocultan la variación temporal entre parejas.
No hay evidencia consistente para atribuir una mejora al ajuste. No se afirma
que sea intrínsecamente más lento ni que este ruido se deba al emulador.

Se retiraron los cambios de clasificación y del bucle: los tres archivos de
producción vuelven a `e7cc68f`. Se reconstruyen Debug y Release para que los
ejecutables finales correspondan al código conservado. No se cambian los
predeterminados ni se fusiona la rama a `main`.

Verificación final tras retirar el ajuste: `ctest --preset debug`, 7/7 suites
en verde; 189 casos / 3562 comprobaciones por cada combinación N64. Runner y
frontend Release recompilados. Revisión independiente del código experimental
y de las conclusiones numéricas sin hallazgos accionables. El commit conserva
solo este informe y las notas de sesión, sin cambios de producción.

Datos locales: `build/rsp-entry-benchmark.json`,
`build/rsp-entry-benchmark-reverse.json` y `build/rsp-entry-analysis.json`.
Procedimiento: `build/benchmark-rsp-entry.ps1`, con y sin `-Reverse`, más
`build/summarize-rsp-entry.ps1` para emparejar ejecuciones consecutivas.

## Próximo alcance

Investigar la ejecución vectorial del RSP, que sigue pasando por C++ incluso
dentro del bloque nativo. Cuantificar frecuencia y coste de opcodes y sus llamadas
antes de elegir un primer grupo para especialización o emisión nativa. Medir ese
grupo contra sus helpers y luego el juego completo, conservando pruebas de
acumuladores, banderas, saturación y tiempos. No ampliar el JIT de CPU ni repetir
la omisión de entradas como una mejora demostrada. Una nueva arquitectura HLE o
GPU exigiría su propio diseño; no se ha implementado como parte de este ensayo.
