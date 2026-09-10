/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   Mocks-espia de la HAL de I2C para el arnes de pruebas de ad5144.
 *
 * ad5144.c se compila SIN modificar contra estos mocks. Ninguno ejecuta
 * logica de bus real: solo registran con que argumentos se les llamo y
 * devuelven un estado configurable, para poder afirmar "el comando X genera
 * exactamente la trama Z en la direccion D" y "un NACK del bus no actualiza
 * el shadow[] ni corta el barrido de canales".
 ******************************************************************************
 */
#include "mock_hal.h"
#include <string.h>

Ad5144HalSpy_t g_spy;

void Ad5144Spy_Reset(void)
{
    memset(&g_spy, 0, sizeof(g_spy));
    g_spy.ready_ret = HAL_OK;
    g_spy.tx_ret    = HAL_OK;
    for (int i = 0; i < AD5144_SPY_MAX_TX; i++) {
        g_spy.tx_ret_seq[i] = -1;   /* sin forzado por llamada */
    }
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                        uint32_t Trials, uint32_t Timeout)
{
    (void)hi2c;
    g_spy.ready_calls++;
    g_spy.ready_addr    = DevAddress;
    g_spy.ready_trials  = Trials;
    g_spy.ready_timeout = Timeout;
    return g_spy.ready_ret;
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                          uint8_t *pData, uint16_t Size, uint32_t Timeout)
{
    (void)hi2c;
    int idx = g_spy.tx_calls;
    g_spy.tx_calls++;

    if (idx < AD5144_SPY_MAX_TX) {
        g_spy.tx_addr[idx]    = DevAddress;
        g_spy.tx_size[idx]    = Size;
        g_spy.tx_timeout[idx] = Timeout;
        g_spy.tx_b0[idx]      = (Size >= 1U && pData != NULL) ? pData[0] : 0U;
        g_spy.tx_b1[idx]      = (Size >= 2U && pData != NULL) ? pData[1] : 0U;

        if (g_spy.tx_ret_seq[idx] != -1) {
            return (HAL_StatusTypeDef)g_spy.tx_ret_seq[idx];
        }
    }
    return g_spy.tx_ret;
}
