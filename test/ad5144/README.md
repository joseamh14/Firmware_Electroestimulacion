# Arnés de pruebas de escritorio — `ad5144` (driver AD5144 I2C)

Valida el driver del potenciómetro digital cuádruple **AD5144** de
`BSP/Src/ad5144.c` **sin hardware ni STM32CubeIDE**: construcción exacta de
la trama I2C, conversión mV→código del wiper, y política de actualización
del `shadow[]`.

En la placa hay dos instancias: **U1** (`ADDR` a VLOGIC → `0x2B`, canales
1‑4) y **U2** (`ADDR` a GND → `0x28`, canales 5‑8).

El fichero de test **incluye `ad5144.c` entero** (para alcanzar la función
`static BuildCommandByte`) y lo enlaza contra mocks‑espía de
`HAL_I2C_IsDeviceReady` y `HAL_I2C_Master_Transmit` (`mocks.c`). Cada
transacción se captura en `g_spy` (dirección, byte 0, byte 1, `Size`,
`Timeout`); el retorno de ambas funciones HAL es forzable, y el de
`HAL_I2C_Master_Transmit` puede forzarse **por número de llamada**
(`tx_ret_seq[]`) para reproducir un fallo a mitad de un barrido de canales.

## Ejecutar

```bash
bash test/ad5144/build_and_run.sh
```

Compilador por defecto: **LCC-win64** (incluido con MATLAB). Override:

```bash
LCC_ROOT="/c/ruta/a/lcc64/lcc64" bash test/ad5144/build_and_run.sh
```

Salida esperada: `=== 272 comprobaciones, 0 fallos ===` / `RESULTADO: OK`
(código de salida 0).

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| Codificación del comando | `AD5144_CMD_WRITE_RDAC` = `0x1`; `BuildCommandByte` → `0x10/0x11/0x12/0x13`; nibble alto (control) siempre `0001`, nibble bajo = canal |
| `AD5144_Init` — NULL | `dev`/`hi2c`/ambos NULL → `HAL_ERROR`; no sondea ni transmite |
| `AD5144_Init` — U1 y U2 | guarda `hi2c` y `addr7`; sondea en `addr7<<1` (`0x56` / `0x50`); `Trials=2`; `Timeout=AD5144_I2C_TIMEOUT_MS`; **no** escribe ningún RDAC; `shadow[0..3]=0` (aunque el handle traiga basura) |
| `AD5144_Init` — constantes | `AD5144_I2C_ADDR_ADDR_VLOG=0x2B`, `AD5144_I2C_ADDR_ADDR_GND=0x28` |
| `AD5144_Init` — fallo de sondeo | `HAL_ERROR`/`HAL_BUSY`/`HAL_TIMEOUT` de `HAL_I2C_IsDeviceReady` se propagan tal cual |
| `AD5144_SetChannelRaw` — NULL | `dev` NULL y `dev->hi2c` NULL → `HAL_ERROR` sin transmitir |
| `AD5144_SetChannelRaw` — trama | los 4 canales a `0x7F` y códigos límite `0`/`255`: byte 0, byte 1, dirección, `Size=2`, `Timeout` exactos; una sola transacción por escritura |
| `AD5144_SetChannelRaw` — `shadow[]` | se actualiza **solo** si la transacción devuelve `HAL_OK`; con `HAL_ERROR`/`HAL_BUSY`/`HAL_TIMEOUT` queda intacto |
| `AD5144_SetChannelMillivolts` — validación | `full_scale_mv=0` → `HAL_ERROR`; `dev` NULL → `HAL_ERROR`; ninguno transmite |
| `AD5144_SetChannelMillivolts` — clamp | `target_mv > full_scale_mv` se limita a fondo de escala (código `255`), no se escala fuera de rango |
| `AD5144_SetChannelMillivolts` — redondeo | 9 puntos con `fs=1000` que redondean **al alza** (`2`,`100`,`500`,`999` mV) y **a la baja** (`1`,`99`,`997` mV), más `fs=2000`; contraste con la fórmula reimplementada en el arnés |
| `AD5144_SetChannelMillivolts` — tope 255 | barrido de ~2 100 puntos sobre 6 escalas (incl. `fs` impar `333` y `fs=65535`): ningún código supera 255, todo coincide con la fórmula, `target ≥ fs` → `255` |
| `AD5144_AllChannelsZero` — camino feliz | 4 transacciones en orden A,B,C,D con byte 1 = 0; `shadow[]` a 0 |
| `AD5144_AllChannelsZero` — fallo a mitad | no corta el barrido (siempre 4 transacciones); `shadow[]` de los canales que fallan queda intacto; devuelve el estado del **último** fallo |
| `AD5144_AllChannelsZero` — NULL | `dev` NULL y `dev->hi2c` NULL → `HAL_ERROR` sin transmitir |

## Verificación del formato de trama contra la datasheet

Contrastado con **AD5124/AD5144/AD5144A Data Sheet, Rev. B**, Tabla 20
("Advance Commands Operation Truth Table") y Tabla 14:

- **Comando 1**, bits de control `C3 C2 C1 C0 = 0 0 0 1` → *"Write contents
  of serial register data to RDAC"*. Bits de dirección `A3..A0` = RDAC de
  destino (`0000`=RDAC1 … `0011`=RDAC4). Segundo byte = dato `D7..D0`.
- El AD5144 es de **256 posiciones**, por lo que el código de wiper usa los
  8 bits (0..255). El AD5124 (128 posiciones) ignoraría el bit 0.

El driver construye `byte0 = (AD5144_CMD_WRITE_RDAC << 4) | (canal & 0x0F)`
con `AD5144_CMD_WRITE_RDAC = 0x01`, y `byte1 = code`. **Coincide
exactamente** con el Comando 1 de la datasheet: el código de instrucción es
correcto, no hay discrepancia.

## Hallazgos documentados (no se corrige nada, solo se anota)

1. **`AD5144_AllChannelsZero` enmascara fallos intermedios.** Recorre los 4
   canales y, ante un fallo, guarda el estado y sigue; el valor devuelto es
   el del **último** canal que falló. Si fallan varios con códigos de error
   distintos (p. ej. `HAL_TIMEOUT` en el canal B y `HAL_BUSY` en el D), el
   llamante solo ve `HAL_BUSY` y no puede saber cuántos ni cuáles canales
   quedaron sin poner a 0. El arnés fija este comportamiento como está
   (`test_all_channels_zero`); si se considera un riesgo para el arranque
   seguro en 0 V, la decisión de cambiarlo es del responsable del proyecto.

2. **`AD5144_Init` deja el handle poblado aunque el sondeo falle.** Si
   `HAL_I2C_IsDeviceReady` devuelve error, `Init` propaga ese estado pero
   ya ha escrito `hi2c` y `addr7` y ha puesto `shadow[]` a 0. Un llamante
   que no compruebe el valor de retorno se queda con un handle de aspecto
   usable sobre un chip que no respondió al sondeo. Menor.

3. **La rama `if (code > 255)` de `AD5144_SetChannelMillivolts` es, hoy,
   inalcanzable.** El clamp previo de `target_mv` a `full_scale_mv` acota el
   resultado de `round(target·255/fs)` a 255 exacto. Es una comprobación
   defensiva redundante; el arnés confirma que el código nunca la necesita
   en todo el barrido.

4. **Nota de documentación (fuera del alcance de este arnés).** El
   comentario de cabecera de `ad5144.h` cita el rango de amplitud "entre 0
   y 1 V", mientras que `CLAUDE.md` fija el límite de software del
   instrumento en 2 V. `full_scale_mv` es un parámetro de llamada (ganancia
   de la etapa ESA), así que no afecta al driver, pero conviene alinear la
   redacción.

## Archivos

- `mock_hal.h` / `stm32l4xx_hal.h` — HAL mínima: `HAL_StatusTypeDef`,
  handle opaco de I2C, prototipos de las dos funciones HAL usadas y el
  struct del espía `g_spy`.
- `mocks.c` — espías de `HAL_I2C_IsDeviceReady` y `HAL_I2C_Master_Transmit`
  (captura de trama + retorno forzable, global y por número de llamada).
- `test_ad5144.c` — incluye `ad5144.c`, casos de prueba + mini‑framework de
  asserts (mismo estilo que `test/dac_waveform/`).
- `build_and_run.sh` — compila y ejecuta.
