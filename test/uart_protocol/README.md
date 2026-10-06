# Arnés de pruebas de escritorio — `uart_protocol`

Valida el parser de comandos de `App/Src/uart_protocol.c` **sin hardware ni
STM32CubeIDE**: compila el parser real contra mocks y ejecuta un binario de
PC que comprueba, comando a comando, qué se llama y qué se responde.

Mocks (mismo patrón que `test/session_fsm/`):

- **`CDC_Transmit_FS`** → espía que sólo acumula lo transmitido (`g_tx`),
  sin tocar USB real. `MockUsb_SetConfigured()` simula pila USB
  lista / no lista.
- **`SessionFSM_Start` / `SetChannelMillivolts` / `SetBankFrequency` /
  `SetWell` / `Stop`** → registran argumentos y devuelven un
  `HAL_StatusTypeDef` configurable (`g_fsm.ret`), sin lógica de sesión.
  `SessionFSM_GetStatus` devuelve una foto fija y **no** cuenta como
  mutación. `SessionFSM_StartParamsValid` reproduce la regla real
  (tiempos dentro de sus límites y fase ON no mayor que la duración total) y
  `SessionFSM_StoreAmplifierSafeDefault` devuelve un estado forzable
  (`g_fsm.eeprom_ret`).
- **`SdLogger_GetStatus`** → devuelve la foto `g_log` (estado, fichero,
  registros, últimas corrientes).
- **`FlashLog_*`** → `FlashLog_Append` cuenta escrituras
  (`g_flash_appends`) y devuelve `g_flash_ret`; `FlashLog_ReadLatest`
  responde "sin registro".
- **`current_calc.c`** se compila **real** (no se simula): los comandos
  de calibración se comprueban contra el módulo de producción.

## Ejecutar

```bash
bash test/uart_protocol/build_and_run.sh
```

Compilador por defecto: **LCC-win64** (incluido con MATLAB). Override con
`LCC_ROOT=...`.

Salida esperada: `=== 163 comprobaciones, 0 fallos ===` / `RESULTADO: OK`
(código de salida 0).

> Al compilar `uart_protocol.c` bajo LCC aparece 1 warning
> (`result of unsigned comparison is constant`, línea del chequeo
> `off_hours < SESSION_OFF_HOURS_MIN` con el mínimo a 0). Es una
> comprobación defensiva de límite inferior; la build real con
> `arm-none-eabi-gcc` sigue en 0 warnings. No se toca desde este arnés.

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| Despacho de comandos | `SET AMP/FREQ/WELL`, `START`, `STOP`, `STATUS` → llamada correcta con argumentos correctos; horas de `START` → segundos; `STATUS` no muta la FSM |
| `ERR SYNTAX` | token no numérico, desbordamiento de `uint32`, argc corto/largo, exceso de tokens, `STOP`/`STATUS` con argumentos, comando/subcomando desconocido, banco ≠ A/B, valor ≠ ON/OFF → **sin** llamar a `session_fsm` |
| `ERR RANGE` | amplitud > 2000, canal ∉ [1,8], frecuencia 0 o > 300000, pocillo ∉ [1,8], `START` con horas_on/off > 168, días 0 o > 365, fase ON más larga que la duración total (`START 48 6 1`: 48 h frente a 24 h) → **sin** llamar a `session_fsm` |
| Case-insensitive | `set amp`, `SeT fReQ a`, `oN`, `StArT`, `status`, `stop` reconocidos |
| Desbordamiento de línea | línea > `UART_LINE_MAX` (64) → `ERR SYNTAX`, no corrompe el siguiente comando; troceada entre dos `Task()`; frontera exacta 64 vs 65 caracteres |
| Fallo de `session_fsm` | mock → `HAL_ERROR` ⇒ `ERR HW` en `SET AMP/FREQ/WELL`, `STOP` y `START` con parámetros válidos; **nunca** `OK` |
| Independencia de USB | con la pila no configurada el comando se procesa igual y la respuesta se descarta sin fallo |
| Calibración | `GET CAL n` con valores por defecto · `SET CAL n g off` con decimales y signo explícito, leído de vuelta con 6/3 decimales · `SET VREF` · `ERR RANGE` por canal, ganancia ∉ (0,10], offset > ±2000 µA, VREF 0 o > 3600 · `ERR SYNTAX` por coma decimal, dos puntos, más de 6 decimales, texto, argumentos de menos/de más · un `SET CAL` rechazado no altera la calibración · `SAVE CAL` escribe un registro en flash y devuelve `ERR HW` si la escritura falla · la calibración no muta la sesión |
| Medida y mantenimiento | `GET CUR` sin registro (`LOG=OFF`, `NAN`), con registro activo (`LOG=ON`, fichero, nº de registros, corrientes con 3 decimales) y sin tarjeta (`LOG=NOSD`) · `EEPROM SAFE` → `OK`, `ERR STATE` si hay sesión en curso (`HAL_BUSY`), `ERR HW` si el chip no confirma · sintaxis incorrecta no toca la EEPROM |

## Archivos

- `mock_hal.h` / `stm32l4xx_hal.h` — tipos HAL mínimos que exigen las
  cabeceras arrastradas por `session_fsm.h`.
- `usbd_cdc_if.h` — shim de la interfaz CDC (handle USB, `USBD_BUSY`,
  `CDC_Transmit_FS`) + controles del espía de transmisión.
- `fsm_spy.h` — estructuras de los espías de `session_fsm`, `sd_logger`
  y `flash_log`.
- `mocks.c` — implementación de los espías (CDC, `SessionFSM_*`,
  `SdLogger_GetStatus`, `FlashLog_*`).
- `test_uart_protocol.c` — casos de prueba + mini-framework de asserts.
- `build_and_run.sh` — compila `uart_protocol.c`, `current_calc.c`,
  `mocks.c` y el test, y ejecuta.
