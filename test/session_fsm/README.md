# Arnés de pruebas de escritorio — `session_fsm`

Valida la lógica de `App/Src/session_fsm.c` **sin hardware ni STM32CubeIDE**:
compila la FSM real contra mocks de la HAL del RTC y de la capa BSP, y
ejecuta un binario de PC que comprueba invariantes.

## Ejecutar

```bash
bash test/session_fsm/build_and_run.sh
```

Compilador por defecto: **LCC-win64**, incluido con MATLAB
(`<MATLABROOT>/sys/lcc64/lcc64`). Si tu instalación está en otra ruta:

```bash
LCC_ROOT="/c/ruta/a/lcc64/lcc64" bash test/session_fsm/build_and_run.sh
```

El código es C99 portable; `gcc` o `clang` sirven igual si se prefiere
(ajustar el script en consecuencia).

Salida esperada: `=== N comprobaciones, 0 fallos ===` / `RESULTADO: OK`
(código de salida 0).

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| Arranque en frío | sin backup → `IDLE`, 0 V en los 8 canales, pocillos desconectados |
| Ciclo ON/REST (`off > 0`) | posición correcta en el ciclo a lo largo del tiempo; reanudación tras un corte que se salta varios tramos; fin de sesión → `IDLE` + backup borrado |
| Estimulación continua (`off = 0`) | nunca entra en `REST`; `elapsed_in_state = total mod on_s` |
| Rango | `START` y setters fuera de rango → `HAL_ERROR` sin aplicar nada; consignas válidas en `IDLE` quedan diferidas |
| Consignas vivas | al entrar en `STIM_ON` se vuelcan 8 amplitudes + 2 frecuencias + 8 pocillos; cambio en caliente en `STIM_ON`; diferido en `STIM_REST` |
| Recuperación de backup válido | retoma el estado y las consignas guardadas; recoloca la posición del ciclo según el tiempo transcurrido real |
| Backup CRC corrupto | → `IDLE` (0 V), backup borrado |
| Backup *magic* inválido | → `IDLE` |
| Parámetros imposibles con CRC válido | → `IDLE` |
| Reloj incoherente (marcha atrás) | en arranque y en marcha → `IDLE`, backup borrado |

## Archivos

- `mock_hal.h` / `stm32l4xx_hal.h` — sustituto mínimo de la HAL (tipos de
  handle, calendario del RTC, `RTC_BKP_DRx`).
- `mocks.c` — reloj falso del RTC, registros de backup con respaldo tipo
  VBAT, y espías de la capa BSP (`AD5144_*`, `DacWaveform_*`,
  `CultureSelector_*`).
- `test_session_fsm.c` — casos de prueba + mini framework de asserts.
- `build_and_run.sh` — compila los 3 `.c` (+ la FSM real) y ejecuta.
