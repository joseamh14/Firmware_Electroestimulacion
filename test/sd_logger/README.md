# Arnés de pruebas de escritorio — `sd_logger`

Valida `App/Src/sd_logger.c` (medida por tramos y registro binario en la
microSD) **sin hardware ni STM32CubeIDE**. FatFs se sustituye por un sistema
de ficheros en RAM (`fatfs.h` / `mocks.c`) que permite quitar la tarjeta o
forzar un error de escritura; el estado de la sesión, el lector de corriente
y el reloj también son simulados. `current_log.c` y `current_calc.c` son los
reales.

## Ejecutar

```bash
bash test/sd_logger/build_and_run.sh
```

Salida esperada: `=== 36 comprobaciones, 0 fallos ===` / `RESULTADO: OK`.

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| Tramos | en IDLE no se mide ni se abre nada; STIM_ON arranca la lectura de los 8 canales y abre `B0001.BIN`; REST e IDLE la paran y cierran el fichero; el tramo siguiente usa `B0002.BIN` |
| Registro | 36 bytes por segundo; marca en ms desde el inicio del tramo (1000, 2000…), que vuelve a empezar en cada tramo; valor ~1000 µA en el canal con medidas y NaN en los demás; `f_sync` cada 10 registros; en REST no se escribe |
| Pausa | cada escritura ocurre con la lectura en pausa, y se reanuda después |
| Sin tarjeta | estado `NOSD`, se sigue midiendo y hay valores para `GET CUR`; con la tarjeta puesta, se registra en el tramo siguiente |
| Error de escritura | se cierra el fichero, estado `NOSD`, la lectura sigue y no queda en pausa |
| Bucle retrasado | tras 5,5 s sin pasar por la tarea se cierra una sola ventana, no cinco |

## Límites

El comportamiento real de la tarjeta (latencia de escritura, SDMMC en modo
sondeo con interrupciones activas) solo puede comprobarse en la placa.
