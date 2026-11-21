# JIT por bloques para el RSP de N64

Estado: aprobado e implementado en `codex/n64-rsp-jit`; experimental. El intérprete
sigue predeterminado porque no se alcanzó la mejora de rendimiento requerida.

## Motivo y decisión propuesta

Mario Kart 64 sigue por debajo del tiempo real. El perfil síncrono actual muestra
11,7 % en `RspRegs::execute_scalar`, 6,7 % en `RspRegs::run`, 5,9 % en memoria
vectorial y 5,7 % en `RspVector::execute_alu`, además de las operaciones por carril.
El RSP es la primera zona que conviene evaluar para recompilación. Estas cifras
son tiempo propio por función; no sumar porcentajes inclusivos de llamadas.

Ya se probaron y retiraron dos ajustes pequeños: MSVC /Ob3 dio solo 0,15 % de
mejora mediana de tiempo real, y despachar vectores directamente desde el bucle
del RSP dio una mediana 4,9 % peor. El RDP en hilo tampoco ayudó. No conservar
ninguno de esos experimentos como si fuera una mejora comprobada.

Propongo compilar bloques del RSP a código x86-64 para ejecutar directamente la
aritmética escalar, en vez de interpretar de nuevo cada instrucción. Es una
decisión técnica del implementador basada en ese perfil, no una preferencia que
el usuario haya especificado. El intérprete actual queda como referencia y vía
de respaldo. El objetivo inicial es comprobar una mejora real; no se promete
que este primer JIT alcance por sí solo los 60 cuadros/s.

## Alcance de la primera entrega

- Un backend JIT opcional del RSP para Windows x64 con Build Tools 2022 y C++20.
- Aritmética/lógica escalar, desplazamientos, inmediatos y accesos escalares DMEM
  emitidos como instrucciones nativas. Misma aritmética de 32 bits y wrapping.
- COP2 y memoria vectorial llaman inicialmente a los helpers C++ existentes con
  el opcode del bloque. No se cambia el cálculo de acumuladores, flags ni divisiones.
- El control de flujo, COP0 y BREAK quedan en el intérprete en esta primera entrega.
  Los bloques nativos terminan antes de esas instrucciones; no es un JIT general
  de CPU VR4300, ni un HLE dependiente de F3DEX/Mario Kart.
- Todo el resto del hardware conserva su ejecución y sincronización actuales.
  RDP síncrono por defecto; el hilo experimental no se activa para las mediciones.

Quedan fuera el JIT de VR4300, generación ARM64, gráficos GPU, SIMD vectorial
nuevo, reconocimiento HLE de microcódigos y cambios en juegos o en sus ROMs.

## Generación de código y dependencia

Usar AsmJit mediante CMake FetchContent, con un SHA completo fijado y licencia
Zlib conservada. La elección exacta del SHA queda al implementador: debe compilar
un caso mínimo en este toolchain y registrarse antes de implementar el backend.
No seguir una rama flotante ni copiar un recompilador de otro emulador.

`asmjit::JitRuntime` administra el código generado y su memoria ejecutable. Si
no se puede crear código, el backend queda deshabilitado para esa consola y
continúa el intérprete, sin pedir privilegios ni cambiar ajustes del sistema.
Los errores de generación son resultados comprobados, no punteros ejecutables
parciales. Se respeta el ABI Windows x64, incluida la alineación de pila y el
espacio de llamadas; los helpers no reciben offsets deducidos de un estado serializado.

La opción CMake `EZY_N64_RSP_JIT` permite excluir la dependencia y todo el código
JIT. En arquitecturas distintas de x86-64 solo se construye el intérprete. No
emitir AVX/SSSE3: la primera entrega usa instrucciones x64/SSE2 disponibles en x64.

Referencias primarias consultadas:
[repositorio y licencia](https://github.com/asmjit/asmjit),
[contrato de JitRuntime](https://asmjit.com/doc/classasmjit_1_1JitRuntime.html).

## Contrato de ejecución y tiempo

Cada bloque contiene como máximo 32 instrucciones consecutivas del IMEM, sin
cruzar el final de 4 KiB. No se encadenan bloques nativos directamente. El bucle
existente del RSP decide si usar el bloque o el intérprete y conserva el control
del reloj local, el planificador y la línea MI.

Antes de cada instrucción nativa se verifica el presupuesto de ciclos disponible,
con la misma paridad de 1/2 ciclos CPU que `short_step_`. El reloj de la consola
no avanza dentro del compilador ni por el tiempo que tarde en generar código.
El bloque devuelve instrucciones retiradas y la siguiente hora local; no llama
a `Scheduler::advance`, no agenda eventos y no puede pasar un evento externo.
El número de instrucciones por salida es explícito: no suponer que siempre se
ejecutó el bloque completo.

Las instrucciones de control se ejecutan mediante la ruta actual. Si el PC está
en un delay slot (`next_pc_` distinto de `sp_pc_ + 4` con máscara 0xFFC), ejecutar
una instrucción interpretada antes de consultar la caché. También tratar como
control la instrucción cuyo efecto sea habilitar o detener el RSP. Esto conserva
los saltos, sus slots, single-step, BREAK y COP0 sin inventar otra representación
de un salto pendiente. La comprobación de single-step desactiva el JIT para ese paso.

Tampoco entrar en código nativo si el RSP fue despachado empatado con otro evento
en `others_bound()`: ejecutar ese paso interpretado conserva el orden de prioridad
actual. Si `stop_on_line` llega con MI ya activo, limitar el tramo a una instrucción,
igual que el bucle de referencia. Con MI inactivo, los opcodes nativos de esta
entrega no pueden elevarlo por su cuenta y no se ejecutan otros eventos en paralelo.

Cada instrucción retirada materializa GPR, PC/next-PC y fase de reloj antes de
una salida posible. Nunca se mantiene un valor de r0 distinto de cero observable
por la siguiente instrucción. El presupuesto y los flags se vuelven a leer al
regresar de un helper. Si no hay suficiente tiempo, volver al bucle sin retirar
una instrucción; ese bucle conserva la misma reprogramación que el intérprete.

Los loads/stores nativos respetan endianness big-endian, extensión de signo y
wrapping por byte en DMEM (`address & 0xFFF`), incluso en cruces de su final.
Un opcode no soportado termina el bloque antes de ejecutarlo y se interpreta
una sola vez. No congelar bucles ni cambiar la precisión para ganar velocidad.

El emisor usa una vista standard-layout con punteros a GPR/DMEM y al estado de
PC/fase. El C++ construye esa vista; no usar offsets del layout privado completo
de `RspRegs`. Los thunks de helpers reciben el objeto mediante un puntero opaco.
El resultado de bloque indica instrucciones retiradas y próxima hora local; el
bucle no vuelve a retirar ni a alternar fase por esas instrucciones.

## Caché, escrituras de IMEM y vida útil

La caché pertenece a una consola y usa PC de entrada más una generación de IMEM.
Una escritura a IMEM invalida toda la caché en la primera versión. Así se evitan
ejecutar microcódigos anteriores cuando un juego cambia de tarea. La memoria
nativa tiene un límite de 16 MiB por consola; alcanzar el límite vacía la caché
en un punto de retorno seguro y permite compilar otra vez.

Eliminar la exposición mutable de `RspRegs::mem()`. Los accesos CPU a SP memory,
incluidos sus espejos, usan métodos acotados de lectura/escritura del RSP; las
escrituras invalidan antes de entregar control al siguiente bloque. DMA hacia
IMEM invalida una vez por fila completada. DMEM no invalida código. Actualizar
también HLE boot y las pruebas que hoy escriben mediante un puntero.

No quedan punteros mutables que puedan escribir IMEM después de invalidar y
antes de ejecutar. El trabajador RDP nunca accede a IMEM. CPU, DMA y RSP JIT
siguen en el mismo hilo: invalidar y liberar solo después de regresar de código
nativo, nunca desde un helper mientras su bloque siga activo.

Guardar no serializa caché, direcciones del host ni código generado. Cargar un
estado, incluido un rollback, invalida la caché y reconstruye el backend desde
IMEM. Conservar la versión 11 y los bytes del estado emulado. Reset y destrucción
liberan la memoria nativa sin referenciar una consola destruida.

## Selección y validación

El runner ofrece `--rsp-interpreter` y `--rsp-jit`, y muestra el backend realmente
activo para que una prueba no pase usando silenciosamente el intérprete. Mantener
el intérprete por defecto hasta que se cumplan los criterios de aceptación.

Primero demostrar equivalencia contra `RspRegs::step` y `run` actuales:

- Todos los opcodes escalares cubiertos, r0 como fuente/destino, alias de registros,
  overflow de 32 bits, cantidades de shift, endianness y wrapping DMEM.
- Salidas en cada límite de ciclo, bloques parciales, PC 0xFFC, single-step,
  branches/delay slots con vectores y BREAK con interrupción.
- Lecturas/escrituras COP0, DMA, eventos externos empatados y cambios de microcódigo
  por CPU, espejos SP, DMA y carga de estado. Volver a un PC ya compilado debe
  ejecutar las instrucciones nuevas, no la caché anterior.
- Guardar, cargar entre backends, estados truncados con rollback, reset,
  destrucción y denegación simulada de memoria ejecutable.
- Los 177 casos N64 actuales pasan con ambos backends; añadir los casos JIT
  necesarios sin sustituir las pruebas existentes.

Comparar estado e imágenes de MK64 a 120/300/600 campos, más continuación desde
estado. Medir al menos tres pares alternados, incluyendo compilación en frío,
con tiempos reales, CPU, tiempo de compilación y proporción de instrucciones
nativas frente a interpretadas. No confundir una mejor microprueba con la mejora
del juego completo.

La prueba ROM histórica es n64-systemtest del commit `196f542`, según
`roms/README.md`: reconstruir o recuperar esa versión y medir primero el
intérprete. Mantener al menos 2984/3721 y comparar también los fallos concretos.
El repositorio original ahora remite a nemu64-test; no comparar sus nuevos
totales con 3721 como si fueran la misma suite. No instalar toolchains globales
sin necesidad; los datos de prueba permanecen ignorados por git.

Activar por defecto solo con equivalencia completa, la suite ROM histórica
validada y al menos 10 % menos tiempo real mediano en MK64. Si falla ese umbral,
documentar qué proporción sigue interpretada y decidir el siguiente alcance
antes de sumar otras optimizaciones. No prometer un multiplicador de velocidad.

## Entrega y autorización

Implementación y validación locales completadas tras la autorización para continuar.
No publicar, fusionar a main ni incluir ROMs sin autorización.

## Resultado de la primera entrega

- AsmJit fijado en `677a10bac2c24f7e068cf1f9e675bf51c7b534ea`;
  bloques de hasta 32 instrucciones y caché invalidada por generación de IMEM.
- Corregida la lectura nula reportada por el usuario: el reset suave del allocator
  fijado conservaba enlaces de árbol obsoletos al liberar varios bloques. Se usa
  `ResetPolicy::kHard` fuera del código nativo, con regresión que reproducía el fallo.
- Debug: 187 casos / 2857 comprobaciones en cada una de las cuatro combinaciones
  RSP intérprete/JIT y RDP síncrono/hilo. Con JIT desactivado: 179 / 1973 en ambos
  modos RDP y sin dependencia AsmJit. Release runner y frontend compilados.
- MK64: imágenes exactas a 120/300/600 campos y continuación de estados entre
  backends. La ROM histórica `196f542` sigue ausente; no se atribuye su baseline
  2984/3721 a esta entrega. Sin cargo/rustup local ni binarios oficiales publicados.
- Tres procesos fríos por modo, alternados, sin otras compilaciones propias:
  intérprete 11,932 / 11,139 / 10,163 s; JIT 50,500 / 45,708 / 43,917 s.
  Medianas: 11,139 frente a 45,708 s (4,10 veces el tiempo). CPU mediana:
  9,500 frente a 39,391 s. Todas las imágenes comparadas fueron idénticas.
- Por 300 campos: 76.828.052 instrucciones nativas y 31.073.143 interpretadas
  (71,2 % nativas), 191.691 bloques compilados y 33,660 s medianos de compilación.
  La recompilación domina; este JIT no resuelve la lentitud. El siguiente alcance
  a evaluar es reutilizar traducciones de microcódigos idénticos entre generaciones
  IMEM, manteniendo la invalidación correcta. No se implementó esa nueva caché.

Los datos locales reproducibles están en `build/rsp-jit-benchmark.json` y
`build/jit-game-validation.log` (ignorados). No activar el JIT por defecto.

## Reutilización de traducciones (2026-11-11)

Continuación autorizada por el usuario: conservar traducciones entre generaciones
de IMEM para reducir el coste de recompilar los mismos microcódigos.

Se conserva la tabla rápida de 1024 entradas de la generación actual. Invalidar
desconecta esa tabla, pero conserva una segunda caché privada de la consola.
Su clave contiene el PC y hasta 128 bytes desde esa dirección (32 instrucciones,
sin cruzar el final de IMEM, con el resto de la clave inicializado a cero).
La comparación exige igualdad exacta de clave, incluso si colisiona el hash.
Incluir bytes posteriores al primer opcode no soportado es conservador: puede
provocar una compilación innecesaria, pero nunca reutilizar instrucciones distintas.

La consulta de contenido ocurre solo al fallar la tabla rápida. Los bloques
reciben la vista actual de registros, DMEM y helpers en cada llamada; no capturan
punteros al estado de una generación anterior. No hay cambios al presupuesto de
ciclos, a los opcodes soportados ni al formato de estados. Cargar o revertir un
estado desconecta la tabla rápida y vuelve a seleccionar por contenido actual.

El límite de código nativo sigue siendo 16 MiB por consola. Al agotarse se hace
reset duro del allocator y se vacían ambas tablas antes de instalar la nueva
traducción, fuera de ejecución nativa. Solo se conservan claves históricas para
bloques compilados con éxito: los opcodes no soportados no acumulan metadatos.
Destruir la consola libera ambas cachés y el runtime. Se añaden contadores de
reutilizaciones y vaciados completos al diagnóstico del runner.

Validación: alternancia A/B/A/B en el mismo PC, cambio dentro del bloque y en su
último opcode, PC distinto con bytes idénticos, final de IMEM y vaciados bajo
presupuesto pequeño. Repetir suites N64 en los cuatro modos, comparar imágenes
MK64 a 120/300/600 campos y continuación de estados en ambas direcciones, y medir
tres procesos fríos por backend en orden alternado. Mantener el intérprete como
predeterminado mientras no se cumpla el criterio completo de aceptación original.

### Resultado de la reutilización

Las regresiones reprodujeron 4 compilaciones en vez de 2 al alternar A/B/A/B,
y 672 en vez de 224 al repetir tres generaciones idénticas. Pasan con la caché.
`ctest --preset debug`: 7/7 suites; N64 189 casos / 3562 comprobaciones en cada
combinación RSP/RDP. Sin JIT: 179 / 1973 en ambos modos RDP, sin AsmJit. ON
restaurado y runner/frontend Release recompilados. Advertencias preexistentes
de sombreado en RDP y truncamiento en una prueba GB; ninguna nueva en la caché.
Revisión independiente sin hallazgos de coherencia, límites o vida útil.
Imágenes MK64 exactas a 120/300/600 campos, más estados completos e imágenes
coincidentes en ambas direcciones de continuación entre backends (300+120).

Tres procesos fríos por backend, orden intérprete/JIT/JIT/intérprete/intérprete/JIT,
RDP síncrono y sin otras compilaciones propias durante la medición:

| Backend | Tiempo real (s), tres ejecuciones | Mediana real (s) | Mediana CPU (s) |
| --- | --- | --- | --- |
| Intérprete | 16,678 / 11,164 / 13,716 | 13,716 | 11,531 |
| JIT con caché | 30,099 / 17,029 / 12,182 | 17,029 | 11,406 |

Los seis procesos compararon exactamente la imagen de 300 campos. El tiempo
real varía mucho; no se atribuye esa variación a la caché. En este lote el JIT
tarda un 24,16 % más de mediana real y consume casi la misma CPU. No se demuestra
la mejora global requerida. No comparar estos tiempos con los de otra fecha
como una medición controlada del cambio.

El efecto sobre compilaciones sí es directo y reproducible: de 191.691 en la
primera entrega a 1.316 compilaciones y 190.375 reutilizaciones por 300 campos,
sin vaciados del presupuesto. Compilación mediana 0,750 s; instrucciones nativas
76.828.052 y fallback 31.073.143, sin cambio. La recompilación ya no domina;
queda perfilar el coste de ejecución nativa, sus helpers y salidas de bloque.

Datos locales en `build/rsp-cache-benchmark.json`, script
`build/benchmark-rsp-cache.ps1`; validación con `build/validate-rsp-cache.ps1`.
La ROM histórica sigue ausente. Conservar el intérprete predeterminado y el JIT
opcional; no se ha demostrado una solución de tiempo real ni se publica la rama.

## Transferencias vectoriales nativas (2026-11-11)

Continuación autorizada tras publicar la rama experimental: se emiten directamente
LBV/LSV/LLV/LDV y sus stores. Una vista opcional de los registros vectoriales
conserva el acceso a datos actuales sin offsets del objeto privado; las vistas
antiguas siguen usando el helper. Accesos agrupados de word/dword, con ruta por
bytes para cruces, preservan endianness, truncamiento y envolturas. No se cambian
acumuladores, banderas, reloj ni estados serializados.

El histograma de 300 campos encontró 16.066.225 transferencias de 2/4/8 bytes.
La versión solo por bytes no mejoró y se refinó antes de conservarla. La versión
agrupada pasa siete suites, 4096 combinaciones de transferencias y equivalencia
de imágenes/estados. Doce procesos fríos contra el JIT anterior, con orden
complementario: CPU favorable en 5/6 parejas, reducción mediana emparejada 8,77 %.
Los tiempos reales varían mucho. Esto apoya una mejora incremental del backend
experimental; no demuestra la mejora requerida frente al intérprete. La ROM
histórica sigue pendiente. Mantener intérprete y RDP síncrono predeterminados.

Informe, método y límites: `docs/performance/2026-11-11-rsp-vector-memory.md`.
