/**
 ******************************************************************************
 * @file    tia_mux.c
 * @brief   Selector de lectura TIA (MAX4581, U7 en SUP) -- multiplexor 8:1.
 ******************************************************************************
 */
#include "tia_mux.h"

#define TIA_MUX_ADDR_MASK  (TIA_MUX_PIN_A0 | TIA_MUX_PIN_A1 | TIA_MUX_PIN_A2)

void TiaMux_Disable(void)
{
    HAL_GPIO_WritePin(TIA_MUX_PORT, TIA_MUX_PIN_EN, TIA_MUX_EN_LEVEL_OFF);
}

HAL_StatusTypeDef TiaMux_Select(uint8_t channel)
{
    if (channel < TIA_MUX_CH_MIN || channel > TIA_MUX_CH_MAX) {
        return HAL_ERROR;
    }

    /* Canal n -> entrada X(n-1): bit0 -> A (PB0), bit1 -> B (PB1), bit2 -> C (PB2) */
    uint32_t idx = (uint32_t)(channel - 1U);
    uint32_t set = 0U;
    if (idx & 0x1U) { set |= TIA_MUX_PIN_A0; }
    if (idx & 0x2U) { set |= TIA_MUX_PIN_A1; }
    if (idx & 0x4U) { set |= TIA_MUX_PIN_A2; }
    uint32_t reset = (uint32_t)TIA_MUX_ADDR_MASK & ~set;

    /* BSRR: mitad baja pone a 1, mitad alta pone a 0, en un solo acceso */
    TIA_MUX_PORT->BSRR = set | (reset << 16U);

    HAL_GPIO_WritePin(TIA_MUX_PORT, TIA_MUX_PIN_EN, TIA_MUX_EN_LEVEL_ON);
    return HAL_OK;
}
