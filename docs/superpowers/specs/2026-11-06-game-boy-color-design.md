# Diseño: soporte de Game Boy Color en el core de GB

Fecha: 2026-11-06 · Rama sugerida: `gb/cgb` (desde `main` cuando entre el PR #1, o desde `gba/header-and-bus` si aún no)

## Intención y por qué

El usuario nunca ha jugado en un Game Boy y quiere poder jugar una variedad de juegos reales:
Pokémon (su favorito), Mario y Zelda, tanto de GB clásico como de GBC. La meta de largo plazo
del proyecto es llegar a GameCube y Wii, así que este trabajo además sirve para practicar la
emulación precisa. Esa última parte es una inferencia, no algo que el usuario haya dicho.
El usuario pidió que la prioridad sea **precisión y compatibilidad**.

Éxito = Pokémon Cristal/Oro/Plata, un Zelda (Link's Awakening DX / Oracle) y un Mario
(Super Mario Bros. Deluxe / Land) arrancan y se juegan, y los juegos de GB clásico se ven con
color como en un GBC real.

## Objetivos

1. **Modo CGB completo** en `src/core/gb`:
   - Detección por header (`0x0143`: `0x80` = compatible, `0xC0` = solo CGB). Cualquier ROM corre
     en hardware CGB por defecto (los juegos DMG entran en el "modo de compatibilidad DMG" del CGB).
   - Opción para forzar el modelo: `auto` (predeterminado, = CGB), `dmg`, `cgb`. Desde CLI del
     frontend y de `gb_runner`.
   - VRAM con 2 bancos (`VBK`, `0xFF4F`), WRAM con 8 bancos de 4 KiB (`SVBK`, `0xFF70`).
   - Paletas de color: `BCPS/BCPD` y `OCPS/OCPD` (`0xFF68-0xFF6B`) con auto-incremento y bloqueo
     de acceso durante el modo 3.
   - Atributos de tiles del BG en el banco 1 de VRAM (paleta, banco de tile, flip X/Y, prioridad).
     Atributos de sprites CGB (banco, paleta 0-7). Reglas de prioridad CGB (LCDC bit 0 como
     "prioridad maestra"). Prioridad de sprites por índice en OAM (`OPRI`, `0xFF6C`).
   - DMA de VRAM: GDMA y HDMA (`0xFF51-0xFF55`), con la temporización y la detención de la CPU.
   - Doble velocidad (`KEY1`, `0xFF4D` + `STOP`): CPU, timer, DIV, serial y OAM DMA al doble;
     PPU, APU (salvo el frame sequencer vinculado a DIV) y HDMA a velocidad normal.
   - Registros sin documentar `0xFF72-0xFF75`, y `PCM12/PCM34` (`0xFF76/77`) de la APU.
   - Puerto infrarrojo `RP` (`0xFF56`): solo el registro, siempre "sin señal". No se emula una
     conexión.
   - Diferencias de hardware CGB que afectan la compatibilidad: valores de registros tras el arranque
     (A=`0x11` le dice al juego que está en un GBC), comportamiento de la APU en CGB (por ejemplo
     los registros de longitud con el APU apagado, la RAM de onda), y no aplicar los bugs que solo tiene la DMG.

2. **Boot ROM simulada (opción A, decidida por el usuario).** No hace falta ningún archivo. Al
   crear la consola se deja la CPU, el hardware y la memoria tal como quedarían tras la boot
   ROM de CGB, en el modo correspondiente:
   - Juego CGB: estado post-boot de CGB.
   - Juego DMG en CGB: modo de compatibilidad DMG (`KEY0`/`0xFF4C` bloqueado en el modo DMG
     y `OPRI`=1). Se elige la paleta automática con la misma lógica que la boot ROM real:
     suma del checksum del título (con licenciatario Nintendo), desempate con la 4.ª letra
     y combinaciones por botón al arrancar (sí entran). Si no hay paleta para el juego, se usa la
     predeterminada de la boot ROM, casi gris, igual que en el hardware real (decidido por el usuario;
     sin paleta de respaldo con color). La paleta
     se carga en las paletas CGB 0 (BG) y 0/1 (OBJ).
   - Si existe `cgb_boot.bin` (ruta por opción), se ejecuta la boot ROM real en lugar de la
     simulación. Opcional, no bloquea nada.

3. **Corrección de color** de la pantalla GBC, aplicada en el frontend al convertir RGB555 →
   RGBA (el core sigue entregando RGB555 crudo). Activada por defecto y con un interruptor para
   apagarla. Fórmula: la de la familia de emuladores conocida (por ejemplo la matriz de Byuu/Near,
   o la de SameBoy "modern balanced"). Elegir una y documentarla en el código.

4. **Save states de GB** (hoy no existen), igual que en GBA: F2 guarda, F4 carga, un slot por
   ROM. Deben cubrir CPU, bus, PPU (incluidos los bancos y paletas CGB), APU, timer, MBC con
   RAM y RTC, y el estado de HDMA y de doble velocidad. Con versión de formato, y rechazo limpio
   de un estado incompatible.

5. **Debugger con soporte CGB**: mostrar el modelo y el modo activos, la velocidad, los bancos
   VBK/SVBK, las paletas BG/OBJ como muestras de color, y el estado de HDMA. Visor de tiles con
   los dos bancos de VRAM.

## Precisión: qué entra y qué no (decisión del asistente)

El PPU actual dibuja por línea y el modo 3 tiene una duración fija. Para este proyecto:
- **Entra:** bloqueo de acceso a VRAM, OAM y paletas durante los modos 2/3, una duración del
  modo 3 aproximada según el scroll, la ventana y los sprites (suficiente para la temporización de
  STAT/HBlank que usan HDMA y muchos juegos), y la temporización correcta de HDMA en HBlank.
- **No entra (decidido por el usuario):** reescribir el PPU como FIFO de píxeles. Será un
  proyecto aparte, después de este. Las pruebas que exigen exactitud al punto (por ejemplo
  varias de Mooneye sobre la temporización del modo 3 y de HBlank) quedan anotadas como fallos
  conocidos y **no** hacen crecer este proyecto.

- El bloqueo de acceso y la duración variable del modo 3 se aplican **también en modo DMG**,
  porque la prioridad es la precisión. Las pruebas DMG se vuelven a correr en cada paso que
  toque el PPU (decisión del asistente).
- Revisión de hardware simulada: **CGB-E** (registros tras el arranque y peculiaridades).
  Decisión del asistente.

## No objetivos

- Super Game Boy, Game Boy Advance en modo GB, accesorios (Game Boy Printer, cámara), link
  cable/infrarrojo entre dos instancias, y las paletas elegidas por el usuario a mano.
- Cargar ROMs comerciales al repositorio. Los dumps son del usuario y van en `roms/`, que está
  en el `.gitignore`.

## Anti-objetivos (fallar aunque "funcione")

- Romper el modo DMG: las pruebas actuales (`cpu_instrs`, `dmg-acid2`, `rtc3test`, tests
  unitarios) deben seguir pasando con `--model dmg`.
- Convertir colores dentro del core o duplicar la ruta de render DMG/CGB por completo; un solo
  PPU con ramas por modo.
- Save states que se cargan "a medias" en silencio cuando el formato no coincide.

## Restricciones

- Compilar solo con los presets de CMake en `build/default` (flujo con varias PCs).
- Código, comentarios y commits en inglés; el estilo existente de `src/core/gb` (C++20,
  `[[nodiscard]]`, `std::span`, comentarios que explican el hardware).
- Seguir el modelo de temporización actual del bus: cada acceso = 1 M-ciclo que avanza el resto.
  En doble velocidad, un M-ciclo de CPU equivale a 2 ciclos de punto del PPU en lugar de 4.

## Verificación

- **Obligatorias para dar el trabajo por terminado:** `cgb-acid2`, `dmg-acid2` (en DMG y en
  CGB-compat), `cpu_instrs` e `instr_timing` en CGB, `boot_regs-cgb`, y los tests unitarios.
  El resto de Mooneye para CGB se corre y se reporta, pero un fallo por temporización al punto
  es aceptable.
- Pruebas automáticas con ROMs de prueba libres (descargarlas a `roms/`, igual que las de GBA):
  `cgb-acid2` (pantalla correcta, comparando contra la imagen de referencia), las pruebas de Blargg
  `cpu_instrs` e `instr_timing` en modo CGB, y las de Mooneye para CGB que apliquen (registros de
  arranque, HDMA y doble velocidad, por ejemplo `boot_regs-cgb`). `dmg-acid2` en modo CGB-compat.
- Tests unitarios nuevos en `tests/gb` para el banco de VRAM/WRAM, el auto-incremento de paletas,
  la longitud y temporización de GDMA/HDMA, el cambio de velocidad, la selección de paleta
  automática y la ida y vuelta de un save state.
- `gb_runner --screenshot` con el modelo como opción, para pruebas sin ventana.
- Juegos comerciales: cuando el usuario copie sus ROMs (pendiente, igual que en GBA).

## Lo que queda al builder

- El orden de los pasos y cómo dividir los commits (uno por pieza de hardware, como en GBA).
- La forma exacta del formato del save state y de la opción del modelo en la CLI.
- La fórmula exacta de corrección de color (entre las dos mencionadas).
