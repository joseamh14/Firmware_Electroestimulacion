# Arnés de pruebas de escritorio — `flash_log`

Valida `BSP/Src/flash_log.c` (registro de solo anexado en la flash interna
que usan la sesión y la calibración) **sin hardware ni STM32CubeIDE**. El
módulo se compila con `-DFLASH_LOG_HOST_TEST`, que sustituye el acceso a la
flash por cuatro funciones de puerto (`FlashPort_*`) implementadas en
`mocks.c` sobre un array en RAM que imita los últimos 32 KB de la flash.

El simulador aplica las reglas que importan para la robustez: solo se
programa una doble palabra borrada, el borrado es por sector de 8 KB, y se
puede programar un **corte de alimentación** en cualquier operación. El corte
deja la doble palabra en curso con un error ECC doble (que en el firmware
activa la NMI) o, en el modo «limpio», borrada, de modo que solo el CRC
puede detectar el registro incompleto. Un borrado interrumpido deja medio
sector con errores ECC.

## Ejecutar

```bash
bash test/flash_log/build_and_run.sh
```

Salida esperada: `=== 90 comprobaciones, 0 fallos ===` / `RESULTADO: OK`.

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| CRC‑32 | vector estándar `"123456789"` → `0xCBF43926` |
| Configuración | `NULL`, registro no múltiplo de 16 o > 256 B, payload que no cabe, marca igual a flash borrada; imagen que invade la región reservada |
| Escritura/lectura | el último leído es siempre el último escrito; secuencia creciente; 8 dobles palabras por registro de 64 B |
| Rotación | el registro 129 borra el sector B sin tocar el A; el 257 borra el A; 519 escrituras sin una sola lectura errónea |
| Cortes | 140 escenarios (7 estados de partida × 10 puntos de corte × 2 modos): nunca se lee un registro a medias, con registro previo siempre se recupera el anterior o el nuevo, y después se puede seguir escribiendo |
| ECC | un registro con una doble palabra ilegible se descarta; un hueco con la primera doble palabra corrupta se salta |
| Secuencia | paso por `0xFFFFFFFF` → `0` sin perder el orden |
| Independencia | sesión y calibración no se borran ni se confunden entre sí |

Se comprobó además que el arnés detecta dos errores introducidos a propósito
en el módulo: borrar el sector activo en lugar del otro, y aceptar registros
sin comprobar el CRC.

## Límites

- La geometría real (páginas de 4 u 8 KB según el bit `DBANK`), la
  programación por doble palabra con la HAL y el comportamiento de la NMI
  por ECC no se pueden probar aquí: dependen del hardware.
