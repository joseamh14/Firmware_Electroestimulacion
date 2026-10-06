/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   Espias de la HAL del ADC, del MAX4581, del DAC y del nucleo para
 *          el arnes de tia_reader y current_log.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "tia_mux.h"
#include "dac_waveform.h"
#include "flash_log.h"
#include <string.h>

TiaSpy_t g_spy;
static int s_arms;

void TiaSpy_Reset(void)
{
    memset(&g_spy, 0, sizeof(g_spy));
    g_spy.start_ret = HAL_OK;
    g_spy.tick = 1000U;
    s_arms = 0;
}

void TiaSpy_FillBurst(uint16_t lo, uint16_t hi)
{
    for (uint32_t k = 0; k < g_spy.len; k++) {
        g_spy.buf[k] = (uint16_t)(lo + ((uint32_t)(hi - lo) * k) / (g_spy.len > 1U ? g_spy.len - 1U : 1U));
    }
}

/* --- HAL del ADC --- */
HAL_StatusTypeDef HAL_ADC_Start_DMA(ADC_HandleTypeDef *hadc, uint32_t *pData, uint32_t Length)
{
    (void)hadc;
    g_spy.starts++;
    g_spy.buf = (uint16_t *)pData;
    g_spy.len = Length;
    if (s_arms < SPY_MAX_ARMS) {
        g_spy.arm_len[s_arms] = Length;
        g_spy.arm_ch[s_arms] = g_spy.mux_ch;
        s_arms++;
    }
    return g_spy.start_ret;
}

HAL_StatusTypeDef HAL_ADC_Stop_DMA(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    g_spy.stops++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *hadc, uint32_t SingleDiff)
{
    (void)hadc; (void)SingleDiff;
    g_spy.calibs++;
    return HAL_OK;
}

uint32_t HAL_GetTick(void) { return g_spy.tick; }

/* --- Capa de nucleo de tia_reader (TIA_READER_HOST_TEST) --- */
void TiaPort_InitTiming(void) { }
void TiaPort_WaitUs(uint32_t us) { g_spy.waits++; g_spy.last_wait_us = us; }
uint32_t TiaPort_IrqSave(void) { g_spy.irq_depth++; return 0U; }
void TiaPort_IrqRestore(uint32_t state) { (void)state; g_spy.irq_depth--; }
void TiaPort_SetTrigger(ADC_HandleTypeDef *hadc, DacBank_t bank)
{
    (void)hadc;
    if (s_arms < SPY_MAX_ARMS) {
        g_spy.trig_bank[s_arms] = (int)bank;   /* se asocia al armado que sigue */
    }
}

/* --- MAX4581 y DAC --- */
void TiaMux_Disable(void) { g_spy.mux_disables++; g_spy.mux_ch = 0; }
HAL_StatusTypeDef TiaMux_Select(uint8_t channel)
{
    if (channel < 1U || channel > 8U) return HAL_ERROR;
    g_spy.mux_selects++;
    g_spy.mux_ch = channel;
    return HAL_OK;
}
uint32_t DacWaveform_GetFrequency(DacBank_t bank) { return g_spy.freq[(int)bank]; }

/* --- flash_log (lo enlaza current_calc; aqui sin flash) --- */
HAL_StatusTypeDef FlashLog_ReadLatest(const FlashLog_Config_t *cfg, void *payload,
                                      uint32_t payload_len, uint32_t *out_seq)
{
    (void)cfg; (void)payload; (void)payload_len; (void)out_seq;
    return HAL_ERROR;
}
HAL_StatusTypeDef FlashLog_Append(const FlashLog_Config_t *cfg, const void *payload,
                                  uint32_t payload_len)
{
    (void)cfg; (void)payload; (void)payload_len;
    return HAL_OK;
}
