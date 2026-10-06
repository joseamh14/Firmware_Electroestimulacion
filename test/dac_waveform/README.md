# Arnés de pruebas de escritorio — `dac_waveform`

Valida, **sin hardware ni STM32CubeIDE**, la aritmética que convierte una
frecuencia objetivo en el par `(prescaler, autorreload)` del temporizador
de disparo del DAC y el aparcado de cada canal en VBIAS (código 2048)
cuando su banco no está generando onda.

La función bajo prueba es `ComputeTimerParams()` de
`BSP/Src/dac_waveform.c`: aritmética pura, no toca ningún registro. Como
es `static`, el fichero de test **incluye `dac_waveform.c` entero**
(técnica habitual para probar funciones internas). Los stubs no-op de
`mocks.c` sólo existen para que `DacWaveform_Init` / `_SetFrequency` /
`_Stop` enlacen y se puedan ejercitar también por el API público, leyendo
el `PSC`/`ARR` resultante en los "registros" (`PSC`/`ARR`/`CNT`) del handle
de timer simulado.

Toda la verificación numérica es entera de 64 bits (sin coma flotante); el
error relativo se expresa en **ppm**.

## Ejecutar

```bash
bash test/dac_waveform/build_and_run.sh
```

Compilador por defecto: **LCC-win64** (incluido con MATLAB). Override:

```bash
LCC_ROOT="/c/ruta/a/lcc64/lcc64" bash test/dac_waveform/build_and_run.sh
```

Salida esperada: `=== 73 comprobaciones, 0 fallos ===` / `RESULTADO: OK`
(código de salida 0).

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| Puntos del Anexo | 1 Hz → PSC=915/ARR=65501 · 100 kHz → 0/599 · 300 kHz → 0/199 |
| Entrada 0 Hz | rechazada (`false`), **no** interpretada como "sin división"; no escribe los `*out` |
| Límite inferior | 1 Hz calculable, ARR y PSC dentro de 16 bits |
| Techo aritmético | `trigger = Fclk` → ARR=0 (máx. sin prescaler) |
| Límite superior útil | vía API público: 300000 Hz pasa, 300001 Hz → `HAL_ERROR` sin tocar el timer |
| Barrido de banda 1 Hz–300 kHz | nunca rechaza · ARR/PSC siempre ≤ 0xFFFF (sin desbordamiento silencioso) · error < 6000 ppm (peor caso ≈ 4998 ppm cerca de 300 kHz, resolución ARR≈200) |
| Barrido por décadas | 10/100/1k/10k Hz: error ≤ 10 ppm (peor: 10 Hz ≈ 6 ppm; los demás exactos) |
| API público | `(PSC,ARR)` llega al timer · `CNT` a 0 en cada reconfiguración · `GetFrequency` coherente · banco inválido → `HAL_ERROR` · 0 Hz para el banco · fallo de `HAL_DAC_Start_DMA` se propaga y el banco no queda activo |
| Onda centrada en VBIAS | `DAC_CODE_MID` = 2048 · excursión ±690 códigos simétrica · `DAC_DIF_PEAK_MV` = 556 mV |
| Aparcado del canal | tras `Init`, `Stop`, 0 Hz y fallo de DMA: DAC habilitado, `TENx` = 0 y DHR = 2048 (salida diferencial nula, nunca flotante) · al arrancar la onda `TENx` vuelve a estar activo antes de `HAL_DAC_Start_DMA` · buffer DMA de 2 muestras HIGH/LOW · el otro banco no se ve afectado |

## Nota sobre el límite superior

El límite de frecuencia **soportado** es `DAC_FREQ_MAX_HZ` = 300 kHz, y lo
hace cumplir `DacWaveform_SetFrequency` antes de llamar a la aritmética.
Por encima de ~119 MHz de frecuencia de disparo, el producto
`trigger·(psc+1)` de `ComputeTimerParams` se sale de `uint32_t` y el
buscador puede devolver un par impreciso; ese camino es **inalcanzable**
desde el API público (filtrado a 300 kHz) y no llega a desbordar el
registro de 16 bits del `ARR`. Queda anotado en el propio test.

## Archivos

- `mock_hal.h` / `stm32l4xx_hal.h` — HAL mínima: `HAL_StatusTypeDef`,
  handles de TIM (con `PSC`/`ARR`/`CNT` observables) y DAC (con registro
  `CR` y bits `TEN1`/`TEN2`), macros `__HAL_TIM_SET_*`, `SET_BIT`/
  `CLEAR_BIT`, constantes `DAC_CHANNEL_*`.
- `mocks.c` — modelo del estado de cada canal del DAC (habilitado, DMA
  activo, DHR, `TENx` en el momento de arrancar el DMA, buffer y longitud)
  y stubs de `HAL_TIM_Base_*`, `HAL_DAC_Start`, `HAL_DAC_SetValue` y
  `HAL_DAC_*_DMA` (+ retorno forzable de `HAL_DAC_Start_DMA`).
- `test_dac_waveform.c` — incluye `dac_waveform.c`, casos de prueba +
  mini-framework de asserts.
- `build_and_run.sh` — compila y ejecuta.
