# RDP en un hilo de trabajo

Estado: propuesta técnica para revisión y aprobación; no implementada.

## Objetivo y contexto

Reducir el tiempo real necesario para ejecutar Mario Kart 64, permitiendo que
CPU/RSP avancen mientras el RDP rasteriza. Se conserva la ROM USA cuyo SHA-1 es
`579c48e211ae952530ffc8738709f078d5dd215e`. El usuario pidió resolver la lentitud y
aceptó continuar con el RDP en otro hilo.

La referencia local es el commit `42a05df`: unos 19,49 s para 300 campos de video
sin ventana, con variación entre ejecuciones. Las 166 pruebas N64 pasan. El perfil
reparte el costo entre CPU, RSP y RDP: este cambio no promete alcanzar 60 campos/s
y no sustituye un futuro JIT/HLE. Las cifras de ~7,3 s en los recordatorios son de
otra PC y no sirven como comparación local.

El alcance es la ejecución concurrente del rasterizador software existente.
No incluye GPU, JIT, HLE de microcódigos, nuevos efectos gráficos ni cambios de ROM.
La imagen y el estado emulado deben coincidir con el modo síncrono.

## Decisiones técnicas propuestas

Son decisiones del implementador, sujetas a esta revisión, no preferencias que
el usuario haya especificado: C++20, un trabajador por consola, cola FIFO acotada,
sin dependencias externas, reservas de RDRAM por páginas de 4 KiB y modo síncrono
conservado como referencia. Ningún acceso concurrente conflictivo queda sin barrera.

### Separación de responsabilidades

El hilo principal conserva el planificador, MI, los registros DPC, los contadores,
la captura de palabras de comandos y los paquetes parciales. `Rdp::step` sigue
consumiendo cada palabra en el mismo ciclo emulado, incluida la selección XBUS.
Cada palabra de DMEM se copia en ese momento: el trabajador nunca recibe un span
vivo de DMEM ni lee registros RSP.

Un componente de rasterización conserva colores, modos, scissor, TMEM, tiles,
combinador, profundidad, bits ocultos y ruido. Solo el trabajador toca ese estado
durante la ejecución asíncrona. El modo síncrono usa el mismo ejecutor de paquetes.

La cola transporta por valor comandos completos, incluidos los de estado y carga
de textura, en orden estricto. Un triángulo tiene hasta 22 palabras; un paquete
parcial permanece en el hilo principal y no se publica antes de completarse.
Los comandos no admitidos mantienen su framing y comportamiento actual.
No se copia el estado completo del rasterizador ni los 8 MiB de RAM por triángulo.

Una sombra mínima de direcciones/modos/scissor en el productor permite calcular
las reservas de memoria sin leer el estado privado del trabajador. Se actualiza
al publicar comandos de estado y se reconstruye después de cargar un estado.

### Propiedad y sincronización de RDRAM

RDRAM sigue siendo el vector estable de 8 MiB del bus. Cada trabajo obtiene un
número de secuencia creciente. Antes de publicarlo, el productor registra las
páginas que puede leer y escribir. Dos tablas, modificadas solo por el principal,
guardan la última secuencia lectora y escritora de cada página. El trabajador
publica su última secuencia completada mediante release; las esperas leen con
acquire. De ese modo, terminar una espera hace visibles las escrituras del dibujo.

Antes de leer una página, el principal espera su última escritura pendiente.
Antes de escribir, espera su última lectura o escritura pendiente. Los accesos
que cruzan página comprueban todas las páginas implicadas. No se suman ciclos
emulados por esperar al sistema operativo.

Reservas inicialmente conservadoras:

- Cargas TMEM/TLUT: lectura de toda RDRAM. Evita reproducir incorrectamente las
  direcciones de carga con wrapping de 24 bits; afinar esos rangos queda fuera.
- Dibujo y relleno: reservar lectura/escritura del rango completo de framebuffer
  que permite el scissor y, cuando corresponda, el rango completo de Z-buffer.
  Incluir alineación, tamaño de píxel y la última fila parcial; limitar a RDRAM.
  Leer/escribir más páginas de las necesarias es válido; omitir una no lo es.
- Estado sin memoria: sin reservas RDRAM.
- Si un cálculo no puede demostrarse seguro, reservar toda RDRAM para ese trabajo.

El cálculo de reservas es una pieza verificable separada: sus rangos deben
contener todos los bytes que el rasterizador toca en las pruebas de bordes.
Los bits ocultos y TMEM no se comparten con CPU/RSP y no necesitan reservas.

Se mantienen los accesos directos de CPU, con una comprobación barata de trabajo
pendiente seguida de la barrera por rango cuando corresponda. Deben cubrirse
`Vr4300::load`, `store`, fetch a través de load y la lectura especial de ocho bytes
en `idle_loop_cycles`; no basta con añadir esperas en `Bus::read32`.
La consulta de trabajo pendiente es metadato del productor, no un acceso sin
sincronizar al estado del trabajador. En modo síncrono toma la vía rápida actual.

También se protegen los lectores de palabras RDP desde RDRAM y los métodos
read/write del bus. Para la primera versión, PI/SI, DMA de SP y los caminos de AI
que leen RDRAM drenan completamente la cola antes de su acceso efectivo. Esto
incluye DMA iniciada por escrituras de registros y la finalización de AI/SP.
El paso escalar/vectorial normal del RSP no requiere drenar: solo accede a DMEM,
IMEM y registros DPC que pertenecen al principal.

La extracción de imagen VI drena antes de leer. Los accesos públicos que exponen
spans de RDRAM se eliminan o encapsulan en operaciones acotadas que drenan y no
permiten conservar una referencia después de otro paso emulado. El único uso
actual en producción es `N64::image`; se puede delegar a `Bus::image`.
Los punteros de acceso directo de CpuBus se consideran internos a una operación
guardada de CPU y no se almacenan entre pasos.

### Orden, registros y puntos de espera

`Sync Full` drena todos los trabajos anteriores antes de limpiar el estado de
pipeline y elevar DP en MI, en el mismo ciclo emulado que en modo síncrono.
El trabajador nunca modifica MI ni agenda eventos. Los sync de Pipe/Tile/Load
conservan el resultado actual mediante el orden FIFO de los comandos.

Leer registros DPC conserva CURRENT, END, flags y contadores del principal,
actualizados según los mismos eventos de consumo que hoy; no lee estado mutable
del rasterizador. Freeze/Flush conservan las reglas actuales de captura y
reanudación y no cancelan retroactivamente comandos ya consumidos.

Las reservas se establecen antes de publicar el paquete. Si la cola está llena,
el productor espera capacidad sin avanzar tiempo emulado. El trabajador puede
procesar varios paquetes por despertar. El tamaño inicial propuesto es 1024
paquetes; ajustar capacidad y tamaño de lote queda al implementador tras medir.
Una cola con mutex y condition_variable es suficiente para la primera versión;
una cola lock-free queda fuera del alcance inicial.

### Estados guardados, errores y ciclo de vida

Guardar drena antes de serializar el planificador, RAM y dispositivos. La cola
del host y los números de secuencia no forman parte del estado emulado.
Cargar drena antes de reemplazar RAM/estado y conserva la restauración
transaccional si el archivo está truncado. El formato versión 11 y el orden de
campos se conservan: separar clases no autoriza cambiar los bytes serializados.
Los comandos parciales, ruido, bits ocultos, TMEM y registros se preservan.
Un estado puede cargarse en cualquiera de los dos modos y producir el mismo
resultado. Los cargadores legacy reinician los datos sin reasignar un trabajador
vivo con `*this = {}`.

Al destruir o reiniciar una consola, se completan los trabajos publicados y se
hace join antes de liberar RDRAM o el estado de dibujo. Los fallos al crear el
hilo dejan disponible el modo síncrono. Los errores del trabajador se capturan,
despiertan a cualquier productor que espere y se propagan al principal; no se
ocultan ni se permite que una excepción abandone el hilo y llame terminate.

### Selección de modo

Una opción interna de creación de N64 selecciona síncrono o trabajador. Los
constructores existentes conservan inicialmente el modo síncrono. El runner
permite seleccionar ambos para pruebas y medición. El frontend solo cambia su
valor por defecto si se cumplen los criterios de corrección y velocidad.
No se añade una preferencia visible ni una dependencia al formato de estados.

## Validación y criterio para conservar el cambio

Antes de modificar la ejecución se captura la referencia síncrona local.
Se compila exclusivamente con los presets del proyecto.

- Las 166 pruebas actuales deben pasar en modo síncrono y con trabajador real.
- Pruebas deterministas, con control del trabajador en lugar de sleeps: orden
  FIFO, capacidad llena, espera de lector/escritor en páginas conflictivas,
  acceso no conflictivo que puede continuar, cruce de página y envolturas.
- Verificar que una modificación de DMEM después del consumo de un comando no
  cambie su contenido encolado; probar paquetes partidos entre escrituras END.
- Ejecutar instrucciones de CPU, incluido el detector de bucle ocioso, que lean
  o escriban framebuffer con trabajo pendiente; probar también fetch y DMA.
- Sync Full no entrega DP hasta que se ven todos los píxeles previos. Comparar
  ciclos de eventos, CURRENT/estado DPC e interrupciones con el modo síncrono.
- Guardar/cargar con cola pendiente, estados cruzados entre modos, carga inválida
  con rollback, reset y destrucción con trabajo pendiente; sin fugas ni bloqueos.
- MK64: imágenes exactas en 120, 300 y 600 campos, más estado tras continuar desde
  una captura. Alternar al menos tres ejecuciones por modo, sin compilación ni
  otras pruebas propias corriendo a la vez; registrar mediana de tiempo real,
  tiempo CPU total de todos los hilos y esperas. El objetivo es tiempo real,
  no reducir CPU total mediante comparaciones engañosas entre hilos.
- n64-systemtest: mantener 2984/3721 cuando se disponga de su ROM. Hoy no está en
  esta PC; marcar explícitamente esta validación como pendiente, no como aprobada.
- ThreadSanitizer solo si hay toolchain compatible disponible; en MSVC no afirmar
  que las pruebas funcionales por sí solas demuestran ausencia de carreras.

Para activar el hilo por defecto se exige salida idéntica, pruebas verdes y al
menos 10 % menos tiempo real mediano en MK64 local. Sin esa ganancia o con regresión,
mantener el modo síncrono por defecto y registrar el resultado experimental. No
seguir sumando cambios sin medir: después correspondería evaluar RSP HLE o JIT.

## Entrega

Implementación y pruebas en una rama `codex/`, recordatorios actualizados con los
resultados y limitaciones, y ejecutable local Release listo para probar. No se
incluye ninguna ROM/ZIP. El commit local anterior sigue sin publicar. Este trabajo
no presupone autorización para enviar commits a `main`; la publicación requiere
la aprobación explícita que pidió la revisión automática en la sesión anterior.
