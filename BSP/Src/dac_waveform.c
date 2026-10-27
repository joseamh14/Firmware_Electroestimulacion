/**
 ******************************************************************************
 * @file    dac_waveform.c
 * @brief   Generacion del pulso bifasico con reposo de estimulacion por
 *          DAC + DMA.
 ******************************************************************************
 */
#include "dac_waveform.h"

/* Buffer circular de DAC_WAVE_SAMPLES = 4 muestras por banco: el DMA vuelca
 * una muestra en cada disparo del timer, de modo que el periodo de la onda
 * son 4 disparos y la frecuencia de disparo es 4 veces la de la onda:
 *   HIGH (T/4)  ->  LOW (T/4)  ->  MID (T/4)  ->  MID (T/4)
 * es decir, fase positiva de T/4, fase negativa de T/4 y reposo en VBIAS
 * durante T/2 (pulso bifasico con reposo de Martin et al., 2024). */
static uint32_t s_bufferA[DAC_WAVE_SAMPLES] = { DAC_CODE_HIGH, DAC_CODE_LOW, DAC_CODE_MID, DAC_CODE_MID };
static uint32_t s_bufferB[DAC_WAVE_SAMPLES] = { DAC_CODE_HIGH, DAC_CODE_LOW, DAC_CODE_MID, DAC_CODE_MID };

typedef struct {
    TIM_HandleTypeDef *htim;
    uint32_t            dac_channel;   /* DAC_CHANNEL_1 o DAC_CHANNEL_2   */
    uint32_t           *buffer;
    uint32_t             current_freq_hz;
    bool                 running;
} DacBankState_t;

static DAC_HandleTypeDef *s_hdac;
static DacBankState_t s_bank[2];

/* Bit TENx (habilitacion de disparo externo) del canal en DAC_CR. Mismo
 * desplazamiento que usa la HAL para el canal 2: (Channel & 0x10). */
static uint32_t TriggerEnableBit(uint32_t dac_channel)
{
    return (uint32_t)(DAC_CR_TEN1 << (dac_channel & 0x10UL));
}

/* Deja el canal del banco con la onda parada y la salida fija en
 * DAC_CODE_MID (Dif = 0). Con TENx = 0 el DAC copia DHR en DOR sin esperar
 * disparo, asi que el valor se aplica aunque el timer este parado. TENx se
 * modifica con el canal deshabilitado (HAL_DAC_Stop_DMA lo deshabilita). */
static void ParkBank(DacBankState_t *st)
{
    HAL_TIM_Base_Stop(st->htim);
    HAL_DAC_Stop_DMA(s_hdac, st->dac_channel);

    CLEAR_BIT(s_hdac->Instance->CR, TriggerEnableBit(st->dac_channel));
    HAL_DAC_SetValue(s_hdac, st->dac_channel, DAC_ALIGN_12B_R, DAC_CODE_MID);
    HAL_DAC_Start(s_hdac, st->dac_channel);

    st->running = false;
}

/* Buscador de (prescaler, autorecarga): toma el menor prescaler para el que
 * ARR+1 cabe en 16 bits. Trabaja sobre la frecuencia de DISPARO (4x la de la
 * onda, DAC_WAVE_SAMPLES). ARR+1 se redondea al entero mas proximo (no se
 * trunca): con 4 disparos por periodo, a 300 kHz ARR+1 vale solo ~100 y el
 * truncado llegaria a ~1 % de error de frecuencia; redondeando queda en
 * <= 0,5 %. */
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
        uint32_t arr_plus_1 = (DAC_TRIGGER_CLOCK_HZ + divisor / 2U) / divisor;
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
        ParkBank(&s_bank[b]);
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
    if (!ComputeTimerParams(freq_hz * DAC_WAVE_SAMPLES, &psc, &arr)) {
        return HAL_ERROR;
    }

    /* Se detiene todo antes de tocar PSC/ARR para evitar un disparo suelto
     * con el periodo a medio configurar. */
    HAL_TIM_Base_Stop(st->htim);
    HAL_DAC_Stop_DMA(s_hdac, st->dac_channel);

    /* Si el banco venia aparcado, se reactiva el disparo por TRGO (el canal
     * esta deshabilitado en este punto, condicion para tocar TENx). */
    SET_BIT(s_hdac->Instance->CR, TriggerEnableBit(st->dac_channel));

    __HAL_TIM_SET_PRESCALER(st->htim, psc);
    __HAL_TIM_SET_AUTORELOAD(st->htim, arr);
    __HAL_TIM_SET_COUNTER(st->htim, 0U);

    HAL_StatusTypeDef status = HAL_DAC_Start_DMA(s_hdac, st->dac_channel, st->buffer,
                                                   DAC_WAVE_SAMPLES, DAC_ALIGN_12B_R);
    if (status != HAL_OK) {
        ParkBank(st); /* no dejar el canal deshabilitado (salida flotante) */
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

    ParkBank(st);
    return HAL_OK;
}

uint32_t DacWaveform_GetFrequency(DacBank_t bank)
{
    if (bank != DAC_BANK_A && bank != DAC_BANK_B) {
        return 0U;
    }
    return s_bank[bank].running ? s_bank[bank].current_freq_hz : 0U;
}
