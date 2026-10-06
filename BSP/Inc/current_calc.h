/**
 ******************************************************************************
 * @file    current_calc.h
 * @brief   Conversion de la lectura del ADC a corriente de celula (uA).
 *
 * Cadena de medida (N_SUP.png, revision de octubre de 2026): transimpedancia
 * de 2 kOhm por pocillo (R17..R24, OPA4172) -> MAX4581 -> etapa OPA320 con
 * ganancia -5,1k/10k = -0,51 y referencia derivada de VBIAS (R27/R28) ->
 * ADC_IN (PC0). Resultado:
 *
 *     ADC_IN = VBIAS + 1020 Ohm * I_CEL   ->   I_CEL = (V_ADC - VBIAS) / 1020 Ohm
 *
 * que coincide con la expresion del apartado 7.13 de la memoria del otro
 * autor. VBIAS = VREF/2 sale del mismo REF19xx que alimenta VREF+, de modo
 * que en codigos de ADC VBIAS es siempre la mitad de escala (2048) sea cual
 * sea el valor real de VREF: el calculo es ratiometrico y solo necesita
 * VREF para pasar de codigos a mV.
 *
 * Sobre el valor nominal se aplica una correccion lineal por canal
 * (I = ganancia * I_nominal + offset) para absorber las tolerancias de la
 * resistencia de TIA de cada pocillo (2 kOhm) y de la etapa comun. Los
 * valores por defecto (ganancia 1, offset 0) son PROVISIONALES: pendientes
 * de calibrar en placa real; no se inventan valores. La calibracion y VREF
 * se guardan en flash interna (flash_log.h, sectores de calibracion) y se
 * fijan desde el host con SET CAL / SAVE CAL (ver uart_protocol.h). El
 * metodo para obtener los valores (carga patron, etc.) queda a criterio de
 * quien disena la etapa de medida.
 ******************************************************************************
 */
#ifndef BSP_INC_CURRENT_CALC_H_
#define BSP_INC_CURRENT_CALC_H_

#include "stm32l4xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CURRENT_CALC_NUM_CH          (8U)
#define CURRENT_CALC_ADC_FULL_SCALE  (4096L)   /* 12 bits                      */
#define CURRENT_CALC_VBIAS_CODE      (2048L)   /* VREF/2, ratiometrico          */
#define CURRENT_CALC_RTRANS_OHM      (1020L)   /* 2 kOhm * 0,51                 */
#define CURRENT_CALC_VREF_MV_DEFAULT (3300U)   /* REF1933; ver CurrentCalc_SetVref */

typedef struct {
    float gain;       /* adimensional, 1.0 = sin correccion */
    float offset_ua;  /* uA, se suma tras aplicar la ganancia */
} CurrentCalc_Cal_t;

/**
 * @brief  Restablece VREF al valor por defecto y la calibracion de los 8
 *         canales a ganancia 1 / offset 0.
 */
void CurrentCalc_Init(void);

/**
 * @brief  Fija la tension real de VREF+ en mV (p. ej. medida con VREFINT o
 *         con un multimetro). HAL_ERROR si es 0 o supera 3600 mV.
 */
HAL_StatusTypeDef CurrentCalc_SetVrefMillivolts(uint16_t vref_mv);

/**
 * @brief  Fija la calibracion de un canal 1..8. HAL_ERROR si el canal esta
 *         fuera de rango, la ganancia no esta en (0, 10] o el offset supera
 *         +/-2000 uA (no se toca nada). No guarda en flash: ver
 *         CurrentCalc_SaveToFlash.
 */
HAL_StatusTypeDef CurrentCalc_SetCalibration(uint8_t channel, float gain, float offset_ua);

/**
 * @brief  Lee la calibracion vigente de un canal 1..8.
 */
HAL_StatusTypeDef CurrentCalc_GetCalibration(uint8_t channel, CurrentCalc_Cal_t *out);

/**
 * @brief  Carga de flash la ultima calibracion guardada (VREF y los 8
 *         canales). Si no hay ninguna valida, deja la configuracion actual
 *         intacta y devuelve HAL_ERROR.
 */
HAL_StatusTypeDef CurrentCalc_LoadFromFlash(void);

/**
 * @brief  Guarda en flash la calibracion vigente (VREF y los 8 canales).
 */
HAL_StatusTypeDef CurrentCalc_SaveToFlash(void);

/**
 * @brief  Valor de VREF en uso, en mV.
 */
uint16_t CurrentCalc_GetVrefMillivolts(void);

/**
 * @brief  Convierte un codigo de ADC (0..4095) del canal 1..8 a uA, con la
 *         calibracion del canal aplicada. Positivo = corriente que entra al
 *         nodo IN_TIA del pocillo (ADC por encima de VBIAS).
 * @retval HAL_ERROR si el canal, el codigo o el puntero no son validos.
 */
HAL_StatusTypeDef CurrentCalc_CodeToMicroamps(uint8_t channel, uint16_t adc_code, float *out_ua);

/**
 * @brief  Igual que CurrentCalc_CodeToMicroamps pero con el codigo en coma
 *         flotante (p. ej. la media de muchas lecturas). Rango 0..4095.
 */
HAL_StatusTypeDef CurrentCalc_CodeFToMicroamps(uint8_t channel, float adc_code, float *out_ua);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_CURRENT_CALC_H_ */
