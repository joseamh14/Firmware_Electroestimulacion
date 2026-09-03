/**
 ******************************************************************************
 * @file    ad5144.c
 * @brief   Driver para el potenciometro digital cuadruple AD5144 (I2C).
 ******************************************************************************
 */
#include "ad5144.h"

#define AD5144_CMD_WRITE_RDAC (0x01U)

static uint8_t BuildCommandByte(uint8_t instruction, uint8_t channel)
{
    return (uint8_t)((instruction << 4) | (channel & 0x0FU));
}

HAL_StatusTypeDef AD5144_Init(AD5144_Handle_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
    if (dev == NULL || hi2c == NULL) {
        return HAL_ERROR;
    }

    dev->hi2c  = hi2c;
    dev->addr7 = addr7;
    for (uint8_t i = 0; i < 4U; i++) {
        dev->shadow[i] = 0U;
    }

    /* Comprobacion de presencia en el bus antes de dar el chip por valido */
    return HAL_I2C_IsDeviceReady(dev->hi2c, (uint16_t)(dev->addr7 << 1), 2U, AD5144_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef AD5144_SetChannelRaw(AD5144_Handle_t *dev, AD5144_Channel_t ch, uint8_t code)
{
    if (dev == NULL || dev->hi2c == NULL) {
        return HAL_ERROR;
    }

    uint8_t frame[2];
    frame[0] = BuildCommandByte(AD5144_CMD_WRITE_RDAC, (uint8_t)ch);
    frame[1] = code;

    HAL_StatusTypeDef status = HAL_I2C_Master_Transmit(dev->hi2c, (uint16_t)(dev->addr7 << 1),
                                                         frame, sizeof(frame), AD5144_I2C_TIMEOUT_MS);
    if (status == HAL_OK) {
        dev->shadow[ch] = code;
    }
    return status;
}

HAL_StatusTypeDef AD5144_SetChannelMillivolts(AD5144_Handle_t *dev, AD5144_Channel_t ch,
                                               uint16_t target_mv, uint16_t full_scale_mv)
{
    if (dev == NULL || full_scale_mv == 0U) {
        return HAL_ERROR;
    }
    if (target_mv > full_scale_mv) {
        target_mv = full_scale_mv; /* clamp por seguridad, nunca por encima de fondo de escala */
    }

    /* code = round(target_mv * 255 / full_scale_mv) */
    uint32_t code = ((uint32_t)target_mv * 255U + (full_scale_mv / 2U)) / full_scale_mv;
    if (code > 255U) {
        code = 255U;
    }

    return AD5144_SetChannelRaw(dev, ch, (uint8_t)code);
}

HAL_StatusTypeDef AD5144_AllChannelsZero(AD5144_Handle_t *dev)
{
    HAL_StatusTypeDef status = HAL_OK;
    AD5144_Channel_t channels[4] = { AD5144_CH_A, AD5144_CH_B, AD5144_CH_C, AD5144_CH_D };

    for (uint8_t i = 0; i < 4U; i++) {
        HAL_StatusTypeDef s = AD5144_SetChannelRaw(dev, channels[i], 0U);
        if (s != HAL_OK) {
            status = s; /* seguimos intentando con el resto de canales aunque uno falle */
        }
    }
    return status;
}
