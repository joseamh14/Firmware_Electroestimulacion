/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   Stubs de escritorio de la HAL de timer/DAC para el arnes de
 *          pruebas de dac_waveform.
 *
 * ComputeTimerParams (aritmetica pura) no llama a ninguna de estas. El resto
 * de dac_waveform.c (DacWaveform_Init / _SetFrequency / _Stop) si, y los
 * espias modelan el estado de cada canal del DAC: habilitado, DMA en marcha,
 * valor de DHR y el bit TENx en el momento de arrancar el DMA.
 ******************************************************************************
 */
#include "mock_hal.h"
#include <string.h>

DacHalSpy_t g_hal;

void DacHalSpy_Reset(void)
{
    memset(&g_hal, 0, sizeof(g_hal));
    g_hal.dac_start_dma_ret = HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Base_Start(TIM_HandleTypeDef *htim)
{
    (void)htim;
    g_hal.tim_start_calls++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_TIM_Base_Stop(TIM_HandleTypeDef *htim)
{
    (void)htim;
    g_hal.tim_stop_calls++;
    return HAL_OK;
}

static int Idx(uint32_t Channel) { return (Channel == DAC_CHANNEL_2) ? 1 : 0; }

HAL_StatusTypeDef HAL_DAC_Start_DMA(DAC_HandleTypeDef *hdac, uint32_t Channel,
                                    uint32_t *pData, uint32_t Length, uint32_t Alignment)
{
    (void)Alignment;
    int i = Idx(Channel);
    g_hal.dac_start_dma_calls++;
    g_hal.ten_at_start_dma[i] = (hdac->Instance->CR & (DAC_CR_TEN1 << (Channel & 0x10U))) != 0U;
    g_hal.dma_buf[i] = pData;
    g_hal.dma_len[i] = Length;
    if (g_hal.dac_start_dma_ret == HAL_OK) {
        g_hal.en[i] = true;
        g_hal.dma[i] = true;
    }
    return g_hal.dac_start_dma_ret;
}

HAL_StatusTypeDef HAL_DAC_Stop_DMA(DAC_HandleTypeDef *hdac, uint32_t Channel)
{
    (void)hdac;
    int i = Idx(Channel);
    g_hal.dac_stop_dma_calls++;
    g_hal.en[i] = false;     /* como la HAL real: deshabilita el canal */
    g_hal.dma[i] = false;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_DAC_Start(DAC_HandleTypeDef *hdac, uint32_t Channel)
{
    (void)hdac;
    g_hal.dac_start_calls++;
    g_hal.en[Idx(Channel)] = true;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_DAC_SetValue(DAC_HandleTypeDef *hdac, uint32_t Channel,
                                   uint32_t Alignment, uint32_t Data)
{
    (void)hdac; (void)Alignment;
    g_hal.dac_setvalue_calls++;
    g_hal.dhr[Idx(Channel)] = Data;
    return HAL_OK;
}
