/**
 ******************************************************************************
 * @file    current_log.h
 * @brief   Calculo del valor por canal de cada ventana de medida y formato
 *          binario del registro de 36 bytes de la microSD.
 *
 * Valor registrado por canal y segundo: la amplitud de pico media de la
 * corriente,
 *
 *     I = ( I(pico+) - I(pico-) ) / 2      [uA]
 *
 * con I(pico+) e I(pico-) calculados (current_calc.h) a partir de la media
 * de los picos positivos y negativos medidos en esa ventana. Al restar los
 * dos picos se cancela cualquier desviacion comun de la referencia (VBIAS,
 * offset de la etapa), y la ganancia de calibracion del canal se aplica a
 * ambos. Un canal sin medidas en la ventana se registra como NaN.
 *
 * Formato del registro (little-endian, 36 bytes, el que interpreta la
 * interfaz de usuario):
 *   [0..3]   uint32  ms desde el inicio del tramo de estimulacion, tomado
 *                    al cerrar la ventana
 *   [4..35]  8 x float32  valor de los canales 1..8 en uA
 ******************************************************************************
 */
#ifndef APP_INC_CURRENT_LOG_H_
#define APP_INC_CURRENT_LOG_H_

#include "tia_reader.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CURRENT_LOG_NUM_CH        (8U)
#define CURRENT_LOG_RECORD_BYTES  (36U)

/**
 * @brief  Valor por canal (uA) de una ventana. NaN si el canal no tiene
 *         medidas o la conversion falla.
 */
void CurrentLog_Compute(const TiaWindow_t *win, float out_ua[CURRENT_LOG_NUM_CH]);

/**
 * @brief  Empaqueta un registro de 36 bytes en little-endian.
 */
void CurrentLog_Pack(uint32_t ms, const float val_ua[CURRENT_LOG_NUM_CH],
                     uint8_t out[CURRENT_LOG_RECORD_BYTES]);

/** NaN de float32 (patron 0x7FC00000), sin depender de math.h. */
float CurrentLog_NaN(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INC_CURRENT_LOG_H_ */
