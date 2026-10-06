/**
 ******************************************************************************
 * @file    current_calc.c
 * @brief   Conversion de la lectura del ADC a corriente de celula (uA).
 ******************************************************************************
 */
#include "current_calc.h"
#include "flash_log.h"
#include <string.h>

#define CURRENT_CALC_VREF_MV_MAX  (3600U)

/* Limites de plausibilidad de la calibracion: una ganancia fuera de (0, 10]
 * o un offset mayor que el fondo de escala de la medida (~1,6 mA) solo
 * pueden ser un error de entrada. La forma de las comparaciones rechaza
 * tambien NaN e infinitos. */
#define CURRENT_CALC_GAIN_MAX       (10.0f)
#define CURRENT_CALC_OFFSET_MAX_UA  (2000.0f)

static int CalValuesValid(float gain, float offset_ua)
{
    return (gain > 0.0f && gain <= CURRENT_CALC_GAIN_MAX &&
            offset_ua >= -CURRENT_CALC_OFFSET_MAX_UA &&
            offset_ua <=  CURRENT_CALC_OFFSET_MAX_UA);
}

static uint16_t          s_vref_mv = CURRENT_CALC_VREF_MV_DEFAULT;
static CurrentCalc_Cal_t s_cal[CURRENT_CALC_NUM_CH];

/* Registro de calibracion en flash: VREF + 8 x (ganancia, offset). */
#define CURRENT_CALC_FLASH_MAGIC  (0x43414C31UL)   /* 'C''A''L''1' */

typedef struct {
    uint32_t          vref_mv;
    CurrentCalc_Cal_t cal[CURRENT_CALC_NUM_CH];
} CurrentCalcFlash_t;

static const FlashLog_Config_t kCalFlash = {
    { FLASH_LOG_CAL_SECTOR_A, FLASH_LOG_CAL_SECTOR_B },
    CURRENT_CALC_FLASH_MAGIC,
    FLASH_LOG_RECORD_SIZE_FOR(sizeof(CurrentCalcFlash_t))
};

void CurrentCalc_Init(void)
{
    s_vref_mv = CURRENT_CALC_VREF_MV_DEFAULT;
    for (uint8_t i = 0; i < CURRENT_CALC_NUM_CH; i++) {
        s_cal[i].gain      = 1.0f;
        s_cal[i].offset_ua = 0.0f;
    }
}

HAL_StatusTypeDef CurrentCalc_SetVrefMillivolts(uint16_t vref_mv)
{
    if (vref_mv == 0U || vref_mv > CURRENT_CALC_VREF_MV_MAX) {
        return HAL_ERROR;
    }
    s_vref_mv = vref_mv;
    return HAL_OK;
}

HAL_StatusTypeDef CurrentCalc_SetCalibration(uint8_t channel, float gain, float offset_ua)
{
    if (channel < 1U || channel > CURRENT_CALC_NUM_CH) {
        return HAL_ERROR;
    }
    if (!CalValuesValid(gain, offset_ua)) {
        return HAL_ERROR;
    }
    s_cal[channel - 1U].gain      = gain;
    s_cal[channel - 1U].offset_ua = offset_ua;
    return HAL_OK;
}

HAL_StatusTypeDef CurrentCalc_GetCalibration(uint8_t channel, CurrentCalc_Cal_t *out)
{
    if (channel < 1U || channel > CURRENT_CALC_NUM_CH || out == NULL) {
        return HAL_ERROR;
    }
    *out = s_cal[channel - 1U];
    return HAL_OK;
}

HAL_StatusTypeDef CurrentCalc_CodeToMicroamps(uint8_t channel, uint16_t adc_code, float *out_ua)
{
    if ((int32_t)adc_code >= CURRENT_CALC_ADC_FULL_SCALE) {
        return HAL_ERROR;
    }
    return CurrentCalc_CodeFToMicroamps(channel, (float)adc_code, out_ua);
}

HAL_StatusTypeDef CurrentCalc_CodeFToMicroamps(uint8_t channel, float adc_code, float *out_ua)
{
    if (channel < 1U || channel > CURRENT_CALC_NUM_CH || out_ua == NULL) {
        return HAL_ERROR;
    }
    if (!(adc_code >= 0.0f && adc_code <= (float)(CURRENT_CALC_ADC_FULL_SCALE - 1))) {
        return HAL_ERROR;
    }

    /* I[uA] = (code - VBIAS_code) * VREF[mV] / 4096 / 1020[Ohm] * 1000 */
    float dv_mv  = (adc_code - (float)CURRENT_CALC_VBIAS_CODE)
                 * (float)s_vref_mv / (float)CURRENT_CALC_ADC_FULL_SCALE;
    float i_nom  = dv_mv * 1000.0f / (float)CURRENT_CALC_RTRANS_OHM;

    const CurrentCalc_Cal_t *c = &s_cal[channel - 1U];
    *out_ua = c->gain * i_nom + c->offset_ua;
    return HAL_OK;
}

uint16_t CurrentCalc_GetVrefMillivolts(void)
{
    return s_vref_mv;
}

HAL_StatusTypeDef CurrentCalc_LoadFromFlash(void)
{
    CurrentCalcFlash_t rec;
    if (FlashLog_ReadLatest(&kCalFlash, &rec, sizeof(rec), NULL) != HAL_OK) {
        return HAL_ERROR;
    }

    /* Se valida todo antes de aplicar nada: o se carga entero o no se carga */
    if (rec.vref_mv == 0U || rec.vref_mv > CURRENT_CALC_VREF_MV_MAX) {
        return HAL_ERROR;
    }
    for (uint8_t i = 0; i < CURRENT_CALC_NUM_CH; i++) {
        if (!CalValuesValid(rec.cal[i].gain, rec.cal[i].offset_ua)) {
            return HAL_ERROR;
        }
    }

    s_vref_mv = (uint16_t)rec.vref_mv;
    memcpy(s_cal, rec.cal, sizeof(s_cal));
    return HAL_OK;
}

HAL_StatusTypeDef CurrentCalc_SaveToFlash(void)
{
    CurrentCalcFlash_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.vref_mv = s_vref_mv;
    memcpy(rec.cal, s_cal, sizeof(rec.cal));
    return FlashLog_Append(&kCalFlash, &rec, sizeof(rec));
}
