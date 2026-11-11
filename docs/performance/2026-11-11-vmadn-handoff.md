# VMADN SIMD — punto de trabajo incompleto para otra PC

Estado: **WIP, sin validación final**. Se publica para trasladar el trabajo;
no es una entrega validada ni una autorización para fusionar a `main`.
Último punto validado anterior: `f22e0a7`.

## Qué está guardado

- Microbenchmark reproducible de VMADN/VMADH/VMADM, con checksum del estado.
- Kernel AVX2 de VMADN, selección CPU/OS y respaldo escalar, con opción CMake
  `EZY_N64_VECTOR_AVX2`. El objeto AVX2 queda fuera del LTCG Release.
- Pruebas directas del kernel y pruebas públicas de selectores, alias, flags y
  estado de división. Opción `--dump-state` del runner para comparar versiones.
- Scripts `tools/n64_runner/benchmark-vmad.ps1` y `snapshot-vmad.ps1`.

La prueba aislada pasó 20480 comprobaciones, después de fallar contra el kernel
vacío. **El despacho final y las pruebas públicas se añadieron después; no hay
una ejecución final exitosa que los valide.** Las últimas compilaciones fallaron
con Lib.exe 1102/OutOfMemory y LNK1201 al escribir el PDB. C: tenía menos de
10 MB libres. No se terminó la medición del candidato ni la suite completa.
El LastTest.log anterior corresponde al trabajo de transferencias vectoriales.

Se incluye `vmad-before-2026-11-11.csv` como registro sintético de la PC original.
Sus tiempos no sirven para comparar rendimiento contra una PC diferente.
Las ROMs, ejecutables y estados de memoria de `build/` permanecen fuera de Git.

## Cómo retomar con consumo controlado

Usar una sola compilación o prueba pesada a la vez. Comprobar espacio libre y
memoria antes de iniciar. No ejecutar Debug y Release en paralelo. Los comandos
siguientes son una guía para ejecutar después, no pasos ya verificados.

En la nueva PC, obtener la rama `codex/n64-rsp-jit` y leer
`pendientes/recordatorios.md`. Reconstruir las referencias escalares **en esa PC**:

```powershell
cmake --preset default -DEZY_N64_VECTOR_AVX2=OFF
cmake --build --preset debug --target ezy_n64_tests --parallel 1
ctest --preset debug -R ezy_n64
cmake --build --preset release --target ezy_n64_runner --parallel 1
cmake --build --preset release --target ezy_rsp_vector_bench --parallel 1
pwsh -NoProfile -File tools/n64_runner/benchmark-vmad.ps1 -Label before
pwsh -NoProfile -File tools/n64_runner/snapshot-vmad.ps1 -Label before
```

En MSBuild, si persiste el problema de recursos, agregar `/nr:false` y
`/p:CL_MPCount=1` después de `--` al comando de compilación. El script de estados
requiere una ROM propia local; acepta `-Rom 'ruta a la ROM'`.

Luego activar AVX2, todavía de forma secuencial:

```powershell
cmake --preset default -DEZY_N64_VECTOR_AVX2=ON
cmake --build --preset debug --target ezy_n64_tests --parallel 1
ctest --preset debug
cmake --build --preset release --target ezy_n64_runner --parallel 1
cmake --build --preset release --target ezy_rsp_vector_bench --parallel 1
pwsh -NoProfile -File tools/n64_runner/benchmark-vmad.ps1 -Label candidate
pwsh -NoProfile -File tools/n64_runner/snapshot-vmad.ps1 -Label candidate
```

Confirmar que realmente se selecciona AVX2 en un equipo compatible. Comparar
checksums y estados completos, imágenes y rendimiento del juego, con ejecuciones
alternadas y sin compilaciones simultáneas. Verificar también el respaldo con
AVX2 desactivado. Resolver fallos antes de considerar el trabajo validado.
El intérprete y RDP síncrono siguen predeterminados; no cambiar a JIT por defecto.
