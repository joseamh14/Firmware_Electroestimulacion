/**
 ******************************************************************************
 * @file    dac_waveform.c
 * @brief   Generacion de la onda cuadrada de estimulacion por DAC + DMA.
 ******************************************************************************
 */
#include "dac_waveform.h"

/* Buffer circular de 2 muestras por banco: el DMA vuelca alternativamente
 * HIGH y LOW en cada disparo del timer, dando una onda cuadrada de 50% de
 * duty cuya frecuencia es la mitad de la frecuencia de disparo. */
static uint32_t s_bufferA[2] = { DAC_CODE_HIGH, DAC_CODE_LOW };
static uint32_t s_bufferB[2] = { DAC_CODE_HIGH, DAC_CODE_LOW };

typedef struct {
    TIM_HandleTypeDef *htim;
    uint32_t            dac_channel;   /* DAC_CHANNEL_1 o DAC_CHANNEL_2   */
    uint32_t           *buffer;
    uint32_t             current_freq_hz;
    bool                 running;
} DacBankState_t;

static DAC_HandleTypeDef *s_hdac;
static DacBankState_t s_bank[2];

/* Mismo buscador de (prescaler, autorecarga) que en la version anterior
 * del driver de frecuencia -- ver justificacion en el historial del
 * proyecto. Trabaja sobre la frecuencia de DISPARO (2x la de la onda). */
static bool ComputeTimerParams(uint32_t trigger_freq_hz, uint32_t *out_psc, uint32_t *out_arr)
{
    if (trigger_freq_hz == 0U) {
        return false;
    }
    for (uint32_t psc = 0U; psc <= 0xFFFFU; psc++) {
        uint32_t divisor = trigger_freq_hz * (psc + 1U);
        if (divisor == 0U) {
            continue;
        }
        uint32_t arr_plus_1 = DAC_TRIGGER_CLOCK_HZ / divisor;
        if (arr_plus_1 == 0U) {
            continue;
        }
        if (arr_plus_1 <= 0x10000UL) {
            *out_psc = psc;
            *out_arr = arr_plus_1 - 1U;
            return true;
        }
    }
    return false;
}

HAL_StatusTypeDef DacWaveform_Init(DAC_HandleTypeDef *hdac,
                                    TIM_HandleTypeDef *htim_bank_a,
                                    TIM_HandleTypeDef *htim_bank_b)
{
    if (hdac == NULL || htim_bank_a == NULL || htim_bank_b == NULL) {
        return HAL_ERROR;
    }

    s_hdac = hdac;

    s_bank[DAC_BANK_A].htim = htim_bank_a;
    s_bank[DAC_BANK_A].dac_channel = DAC_CHANNEL_1;
    s_bank[DAC_BANK_A].buffer = s_bufferA;

    s_bank[DAC_BANK_B].htim = htim_bank_b;
    s_bank[DAC_BANK_B].dac_channel = DAC_CHANNEL_2;
    s_bank[DAC_BANK_B].buffer = s_bufferB;

    for (uint8_t b = 0; b < 2U; b++) {
        s_bank[b].current_freq_hz = 0U;
        s_bank[b].running = false;
        HAL_TIM_Base_Stop(s_bank[b].htim);
        HAL_DAC_Stop_DMA(s_hdac, s_bank[b].dac_channel);
    }

    return HAL_OK;
}

HAL_StatusTypeDef DacWaveform_SetFrequency(DacBank_t bank, uint32_t freq_hz)
{
    if (bank != DAC_BANK_A && bank != DAC_BANK_B) {
        return HAL_ERROR;
    }
    DacBankState_t *st = &s_bank[bank];
    if (st->htim == NULL || s_hdac == NULL) {
        return HAL_ERROR; /* DacWaveform_Init no llamado todavia */
    }

    if (freq_hz == 0U) {
        return DacWaveform_Stop(bank);
    }
    if (freq_hz < DAC_FREQ_MIN_HZ || freq_hz > DAC_FREQ_MAX_HZ) {
        return HAL_ERROR;
    }

    uint32_t psc, arr;
    if (!ComputeTimerParams(freq_hz * 2U, &psc, &arr)) {
        return HAL_ERROR;
    }

    /* Se detiene todo antes de tocar PSC/ARR para evitar un disparo suelto
     * con el periodo a medio configurar. */
    HAL_TIM_Base_Stop(st->htim);
    HAL_DAC_Stop_DMA(s_hdac, st->dac_channel);

    __HAL_TIM_SET_PRESCALER(st->htim, psc);
    __HAL_TIM_SET_AUTORELOAD(st->htim, arr);
    __HAL_TIM_SET_COUNTER(st->htim, 0U);

    HAL_StatusTypeDef status = HAL_DAC_Start_DMA(s_hdac, st->dac_channel, st->buffer, 2U,
                                                   DAC_ALIGN_12B_R);
    if (status != HAL_OK) {
        return status;
    }
    HAL_TIM_Base_Start(st->htim);

    st->current_freq_hz = freq_hz;
    st->running = true;
    return HAL_OK;
}

HAL_StatusTypeDef DacWaveform_Stop(DacBank_t bank)
{
    if (bank != DAC_BANK_A && bank != DAC_BANK_B) {
        return HAL_ERROR;
    }
    DacBankState_t *st = &s_bank[bank];
    if (st->htim == NULL || s_hdac == NULL) {
        return HAL_ERROR;
    }

    HAL_TIM_Base_Stop(st->htim);
    HAL_DAC_Stop_DMA(s_hdac, st->dac_channel);
    st->running = false;
    return HAL_OK;
}

uint32_t DacWaveform_GetFrequency(DacBank_t bank)
{
    if (bank != DAC_BANK_A && bank != DAC_BANK_B) {
        return 0U;
    }
    return s_bank[bank].running ? s_bank[bank].current_freq_hz : 0U;
}
