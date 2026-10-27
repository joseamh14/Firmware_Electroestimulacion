# Arnés de pruebas de escritorio — `ad5144` (driver AD5144 I2C)

Valida el driver del potenciómetro digital cuádruple **AD5144** de
`BSP/Src/ad5144.c` **sin hardware ni STM32CubeIDE**: construcción exacta de
la trama I2C, conversión mV→código del wiper con el modelo de reostato de
la etapa ESA, y política de actualización del `shadow[]`.

En la placa hay dos instancias: **U1** (`ADDR` a 2V5 = VLOGIC → `0x28`,
canales 1‑4) y **U2** (`ADDR` a GND → `0x2B`, canales 5‑8), según la tabla
15 de la datasheet (TSSOP‑20). Las versiones anteriores del driver tenían
estas dos constantes intercambiadas.

Desde la revisión de esquemáticos de octubre de 2026 el AD5144 trabaja como
reóstato en la realimentación de un inversor de ganancia R_AW/2,5 kΩ
(terminal A a la entrada inversora, W y B a la salida). La relación
código→amplitud es inversa: el código 0 da la ganancia máxima (~4) y el 255
la mínima, que es la posición segura.

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

Salida esperada: `=== 251 comprobaciones, 0 fallos ===` / `RESULTADO: OK`
(código de salida 0). El arnés usa `fabs()` de `<math.h>`; con gcc hay que
añadir `-lm` al enlazar.

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| Codificación del comando | `AD5144_CMD_WRITE_RDAC` = `0x1`; `BuildCommandByte` → `0x10/0x11/0x12/0x13`; nibble alto (control) siempre `0001`, nibble bajo = canal |
| `AD5144_Init` — NULL | `dev`/`hi2c`/ambos NULL → `HAL_ERROR`; no sondea ni transmite |
| `AD5144_Init` — U1 y U2 | guarda `hi2c` y `addr7`; sondea en `addr7<<1` (`0x50` / `0x56`); `Trials=2`; `Timeout=AD5144_I2C_TIMEOUT_MS`; **no** escribe ningún RDAC; `shadow[0..3]=128` (mitad de escala que carga la EEPROM de fábrica) |
| `AD5144_Init` — constantes | `AD5144_I2C_ADDR_ADDR_VLOG=0x28`, `AD5144_I2C_ADDR_ADDR_GND=0x2B` (tabla 15 de la datasheet) |
| `AD5144_Init` — fallo de sondeo | `HAL_ERROR`/`HAL_BUSY`/`HAL_TIMEOUT` de `HAL_I2C_IsDeviceReady` se propagan tal cual |
| `AD5144_SetChannelRaw` — NULL | `dev` NULL y `dev->hi2c` NULL → `HAL_ERROR` sin transmitir |
| `AD5144_SetChannelRaw` — trama | los 4 canales a `0x7F` y códigos límite `0`/`255`: byte 0, byte 1, dirección, `Size=2`, `Timeout` exactos; una sola transacción por escritura |
| `AD5144_SetChannelRaw` — `shadow[]` | se actualiza **solo** si la transacción devuelve `HAL_OK`; con `HAL_ERROR`/`HAL_BUSY`/`HAL_TIMEOUT` queda intacto |
| `AD5144_CodeForMillivolts` — validación | `in_peak_mv=0` o `out_code` NULL → `HAL_ERROR` |
| Modelo de reóstato | relación inversa; puntos calculados a mano (1000 mV → 142, 2000 mV → 27 con `in_peak`=500 mV y R_W = 55 Ω → D = 129 y D = 1); 0 mV y valores por debajo del mínimo (~19 mV) → 255; máximo alcanzable exacto (2011 mV → 0) |
| Objetivo inalcanzable | `HAL_ERROR` sin transmitir y con el `shadow[]` intacto (no se recorta en silencio) |
| Barrido | 0…2000 mV en pasos de 1 mV para tres excursiones de entrada: código monótono y error de salida ≤ medio paso frente a un modelo en doble precisión independiente del driver |
| Margen de diseño | con la excursión de la placa (500 mV) el paso es 7,81 mV, el mínimo activo ≈ 18,8 mV y el máximo nominal ≈ 2,01 V (cifras de la memoria de la etapa analógica); con R_AB −8 % el código 0 da ≈ 1,85 V (−7,5 % respecto a 2 V, dentro del ≈ 15 % sin calibrar) y con R_AB +8 % no supera los ±2,5 V de los terminales |
| `AD5144_AllChannelsZero` — camino feliz | 4 transacciones en orden A,B,C,D con byte 1 = 255; `shadow[]` a 255; nunca escribe el código 0 |
| `AD5144_AllChannelsZero` — fallo a mitad | no corta el barrido (siempre 4 transacciones); `shadow[]` de los canales que fallan queda intacto; devuelve el estado del **último** fallo |
| EEPROM — trama | Comando 11 de la datasheet: byte 0 = `0x80 | canal`, byte 1 = dato; no altera el `shadow[]` del RDAC |
| EEPROM — sondeo por ACK | un intento por ms; 20 NACK → 21 sondeos y 20 ms; sin ACK → `HAL_TIMEOUT` a los 60 ms; trama fallida → sin sondeo |
| `AD5144_StoreSafePowerUp` | 255 en la EEPROM de A, B, C y D en orden; un fallo a mitad no corta el resto |
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

3. *(Obsoleto tras el cambio a modelo de reóstato: la antigua rama
   `if (code > 255)` ya no existe.)*

4. *(Resuelto: la cabecera de `ad5144.h` se reescribió con la topología
   de reóstato y ya no cita un rango de amplitud distinto del límite de
   software de 2 V.)*

## Archivos

- `mock_hal.h` / `stm32l4xx_hal.h` — HAL mínima: `HAL_StatusTypeDef`,
  handle opaco de I2C, prototipos de las dos funciones HAL usadas y el
  struct del espía `g_spy`.
- `mocks.c` — espías de `HAL_I2C_IsDeviceReady` y `HAL_I2C_Master_Transmit`
  (captura de trama + retorno forzable, global y por número de llamada).
- `test_ad5144.c` — incluye `ad5144.c`, casos de prueba + mini‑framework de
  asserts (mismo estilo que `test/dac_waveform/`).
- `build_and_run.sh` — compila y ejecuta.
