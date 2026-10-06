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

Salida esperada: `=== 179 comprobaciones, 0 fallos ===` / `RESULTADO: OK`
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
| Copia en flash — puntos de control | uno cada 60 s con sesión armada (1440 en 24 h), además de en cada transición y cambio de consigna; ninguno en arranque en frío |
| Copia en flash — corte total | backup borrado y calendario a 2000‑01‑01: retoma estado y consignas con ≤ 60 s perdidos; el tiempo sin alimentación no cuenta; reconstruye el backup |
| Copia en flash — prioridades | `STOP` deja registro `IDLE` (no resucita); `STOP` sin sesión no escribe; backup válido manda sobre la flash; backup corrupto con flash válida → reanuda; registro imposible o sesión ya cumplida → `IDLE`; un fallo de escritura no detiene la estimulación |
| Mantenimiento EEPROM | solo en `IDLE`; con sesión (`ON` o `REST`) → `HAL_BUSY` sin tocar la EEPROM |
| Seguridad de salida | pocillo de la máscara con 0 mV → a GND (el AD5144 no llega a 0 V); se conecta/desconecta al cambiar la amplitud en caliente; `SET WELL ON` con 0 mV no conecta; canal con escritura fallida en el AD5144 → desconectado; `START` con fallo de hardware → sesión cancelada, salida segura y sin backup ni copia activa; consigna o frecuencia que fallan en caliente → se conserva el valor anterior y no se persiste |

El CRC‑32 del backup usa ahora el polinomio estándar (`0xEDB88320`); la
versión anterior tenía una errata (`0xEDB88520`) que no impedía detectar
corrupciones pero no era el CRC‑32 habitual.

En los casos de backup corrupto, *magic* inválido y parámetros imposibles se
vacía también la copia en flash, para seguir probando la validación del
backup de forma aislada.

## Archivos

- `mock_hal.h` / `stm32l4xx_hal.h` — sustituto mínimo de la HAL (tipos de
  handle, calendario del RTC, `RTC_BKP_DRx`).
- `mocks.c` — reloj falso del RTC, registros de backup con respaldo tipo
  VBAT, `flash_log` simulado (guarda el último registro y persiste entre
  reinicios), y espías de la capa BSP (`AD5144_*`, `DacWaveform_*`,
  `CultureSelector_*`).
- `test_session_fsm.c` — casos de prueba + mini framework de asserts.
- `build_and_run.sh` — compila los 3 `.c` (+ la FSM real) y ejecuta.
