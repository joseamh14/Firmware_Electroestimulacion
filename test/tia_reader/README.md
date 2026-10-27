# Arnés de pruebas de escritorio — `tia_reader` y `current_log`

Valida la lógica de `BSP/Src/tia_reader.c` (ráfagas del ADC1 sincronizadas
con la onda del DAC) y de `App/Src/current_log.c` (valor por canal y formato
del registro de 36 bytes) **sin hardware ni STM32CubeIDE**.

`tia_reader.c` se compila con `-DTIA_READER_HOST_TEST`: la espera en µs, la
sección crítica y la selección del disparo del ADC las aportan los espías de
`mocks.c`, junto con la HAL del ADC, el MAX4581 (`TiaMux_*`) y la frecuencia
de cada banco (`DacWaveform_GetFrequency`). El arnés completa cada ráfaga a
mano, rellenando el buffer que recibió `HAL_ADC_Start_DMA` y llamando a
`TiaReader_OnBurstComplete()` como haría la interrupción del DMA.

## Ejecutar

```bash
bash test/tia_reader/build_and_run.sh
```

Salida esperada: `=== 86 comprobaciones, 0 fallos ===` / `RESULTADO: OK`.

## Qué se cubre

| Bloque | Comprobación |
|---|---|
| Plan de medida | 0 Hz → no se mide; 100 kHz → ráfaga de 62 muestras (un periodo + 8); 300 kHz → 26; límite exacto 2615/2614 Hz entre «un periodo» (buffer de 2048 muestras) y «flancos»; barrido 1 Hz–300 kHz: la ráfaga siempre cubre el periodo y cabe en el buffer, y en «flancos» el cuarto de periodo (separación entre disparos) es mayor que la ráfaga |
| Rotación | 1,2,…,8 con máscara completa y parcial |
| Modo periodo | un armado por ms como máximo; disparo TIM1 para 1–4 y TIM8 para 5–8; mux cambiado y espera de asentamiento antes de cada armado; máximo y mínimo acumulados por canal; `TakeWindow` vacía el acumulado |
| Banco parado | sus canales se saltan; con los dos parados no se arma nada |
| Flancos (baja frecuencia) | cuatro ráfagas por periodo (pulso bifasico con reposo): la interrupción rearma para el disparo siguiente sin tocar el mux; se combinan el máximo y el mínimo de las cuatro; sin arrastre entre periodos; un rearme fallido a mitad de periodo descarta la medida |
| Ráfaga que no llega | se aborta a los 2 periodos + 20 ms y se pasa al canal siguiente, sin acumular |
| Pausa / parada | la pausa aborta la ráfaga pendiente; interrupción tardía ignorada; parado no arma y deshabilita el MAX4581; fallo de `HAL_ADC_Start_DMA` se reintenta |
| `current_log` | (I⁺ − I⁻)/2 ≈ 1000 µA para ±1266 códigos; un desplazamiento común no afecta; la ganancia de calibración se aplica y el offset se cancela; canal sin medidas → NaN; registro little‑endian (`1.0f` = `00 00 80 3F`, NaN = `00 00 C0 7F`) |

El arnés encontró un error en la primera versión: tras `TiaReader_Start` no
se podía armar en el mismo milisegundo en que se había armado la última vez,
por lo que el primer canal de un tramo podía retrasarse 1 ms. Corregido.

## Límites

No se puede probar aquí lo que depende del silicio: que el ADC arranque
realmente en el flanco con disparo externo y modo continuo, los 5,33 Msps
con el reloj a 80 MHz, el tiempo de habilitación del ADC entre ráfagas y el
asentamiento real del MAX4581 a ±3,3 V.
