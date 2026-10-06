/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   Stubs de escritorio de la HAL de timer/DAC para el arnes de
 *          pruebas de dac_waveform.
 *
 * ComputeTimerParams (la funcion bajo prueba, aritmetica pura) NO llama a
 * ninguna de estas. Existen solo para que el resto de dac_waveform.c
 * (DacWaveform_Init / _SetFrequency / _Stop) enlace y se pueda ejercitar
 * tambien por su API publico.
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

HAL_StatusTypeDef HAL_DAC_Start_DMA(DAC_HandleTypeDef *hdac, uint32_t Channel,
                                    uint32_t *pData, uint32_t Length, uint32_t Alignment)
{
    (void)hdac; (void)Channel; (void)pData; (void)Length; (void)Alignment;
    g_hal.dac_start_dma_calls++;
    return g_hal.dac_start_dma_ret;
}

HAL_StatusTypeDef HAL_DAC_Stop_DMA(DAC_HandleTypeDef *hdac, uint32_t Channel)
{
    (void)hdac; (void)Channel;
    g_hal.dac_stop_dma_calls++;
    return HAL_OK;
}
