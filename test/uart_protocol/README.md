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
  mutación.

## Ejecutar

```bash
bash test/uart_protocol/build_and_run.sh
```

Compilador por defecto: **LCC-win64** (incluido con MATLAB). Override con
`LCC_ROOT=...`.

Salida esperada: `=== N comprobaciones, 0 fallos ===` / `RESULTADO: OK`
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
| `ERR RANGE` | amplitud > 2000, canal ∉ [1,8], frecuencia 0 o > 300000, pocillo ∉ [1,8], `START` con horas_on/off > 168, días 0 o > 365 → **sin** llamar a `session_fsm` |
| Case-insensitive | `set amp`, `SeT fReQ a`, `oN`, `StArT`, `status`, `stop` reconocidos |
| Desbordamiento de línea | línea > `UART_LINE_MAX` (64) → `ERR SYNTAX`, no corrompe el siguiente comando; troceada entre dos `Task()`; frontera exacta 64 vs 65 caracteres |
| Fallo de `session_fsm` | mock → `HAL_ERROR` ⇒ `ERR HW` (y `ERR RANGE` en `START`, por diseño); **nunca** `OK` |
| Independencia de USB | con la pila no configurada el comando se procesa igual y la respuesta se descarta sin fallo |

## Archivos

- `mock_hal.h` / `stm32l4xx_hal.h` — tipos HAL mínimos que exigen las
  cabeceras arrastradas por `session_fsm.h`.
- `usbd_cdc_if.h` — shim de la interfaz CDC (handle USB, `USBD_BUSY`,
  `CDC_Transmit_FS`) + controles del espía de transmisión.
- `fsm_spy.h` — estructura del espía de `session_fsm`.
- `mocks.c` — implementación de los espías (CDC + `SessionFSM_*`).
- `test_uart_protocol.c` — casos de prueba + mini-framework de asserts.
- `build_and_run.sh` — compila los 3 `.c` y ejecuta.
