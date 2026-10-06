/**
 ******************************************************************************
 * @file    ad5144.c
 * @brief   Driver para el potenciometro digital cuadruple AD5144 (I2C).
 ******************************************************************************
 */
#include "ad5144.h"

#define AD5144_CMD_WRITE_RDAC   (0x01U)   /* Comando 1                   */
#define AD5144_CMD_WRITE_EEPROM (0x08U)   /* Comando 11: dato -> EEPROM  */

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
        /* Al encenderse el chip carga su EEPROM (mitad de escala de fabrica) */
        dev->shadow[i] = AD5144_CODE_POWERUP;
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

HAL_StatusTypeDef AD5144_CodeForMillivolts(uint16_t target_mv, uint16_t in_peak_mv,
                                           uint8_t *out_code)
{
    if (out_code == NULL || in_peak_mv == 0U) {
        return HAL_ERROR;
    }

    /* R_AW necesaria = target * R_IN / in_peak. Se descuenta R_W y se pasa
     * a pasos de R_AB/256:
     *   pasos = (R_AW - R_W) * 256 / R_AB
     *         = (target*R_IN*256 - R_W*256*in_peak) / (R_AB*in_peak)
     * redondeado al entero mas proximo. D = 256 - pasos. */
    int64_t num = (int64_t)target_mv * AD5144_RIN_OHM * AD5144_NUM_STEPS
                - (int64_t)AD5144_RW_OHM * AD5144_NUM_STEPS * (int64_t)in_peak_mv;
    int64_t den = (int64_t)AD5144_RAB_OHM * (int64_t)in_peak_mv;

    int64_t steps;
    if (num <= 0) {
        steps = 0;
    } else {
        steps = (num + den / 2) / den;
    }

    if (steps > AD5144_NUM_STEPS) {
        /* Haria falta un codigo negativo: mas ganancia de la que hay. */
        return HAL_ERROR;
    }
    if (steps < 1) {
        /* Por debajo del minimo alcanzable (R_AB/256 + R_W): posicion segura. */
        *out_code = (uint8_t)AD5144_CODE_MIN_GAIN;
        return HAL_OK;
    }

    *out_code = (uint8_t)(AD5144_NUM_STEPS - steps);
    return HAL_OK;
}

HAL_StatusTypeDef AD5144_SetChannelMillivolts(AD5144_Handle_t *dev, AD5144_Channel_t ch,
                                               uint16_t target_mv, uint16_t in_peak_mv)
{
    if (dev == NULL) {
        return HAL_ERROR;
    }

    uint8_t code;
    if (AD5144_CodeForMillivolts(target_mv, in_peak_mv, &code) != HAL_OK) {
        return HAL_ERROR; /* fuera de alcance: no se toca el chip */
    }

    return AD5144_SetChannelRaw(dev, ch, code);
}

HAL_StatusTypeDef AD5144_AllChannelsZero(AD5144_Handle_t *dev)
{
    HAL_StatusTypeDef status = HAL_OK;
    AD5144_Channel_t channels[4] = { AD5144_CH_A, AD5144_CH_B, AD5144_CH_C, AD5144_CH_D };

    for (uint8_t i = 0; i < 4U; i++) {
        /* Posicion segura = ganancia minima (codigo 255), ver ad5144.h */
        HAL_StatusTypeDef s = AD5144_SetChannelRaw(dev, channels[i], (uint8_t)AD5144_CODE_MIN_GAIN);
        if (s != HAL_OK) {
            status = s; /* seguimos intentando con el resto de canales aunque uno falle */
        }
    }
    return status;
}

HAL_StatusTypeDef AD5144_StoreEepromCode(AD5144_Handle_t *dev, AD5144_Channel_t ch, uint8_t code)
{
    if (dev == NULL || dev->hi2c == NULL) {
        return HAL_ERROR;
    }

    uint8_t frame[2];
    frame[0] = BuildCommandByte(AD5144_CMD_WRITE_EEPROM, (uint8_t)ch);
    frame[1] = code;

    HAL_StatusTypeDef status = HAL_I2C_Master_Transmit(dev->hi2c, (uint16_t)(dev->addr7 << 1),
                                                         frame, sizeof(frame), AD5144_I2C_TIMEOUT_MS);
    if (status != HAL_OK) {
        return status;
    }

    /* Sondeo por ACK: mientras programa la EEPROM el chip no responde.
     * HAL_I2C_IsDeviceReady no espera entre intentos, asi que se hace un
     * intento por milisegundo hasta AD5144_EEPROM_TIMEOUT_MS. */
    uint32_t t0 = HAL_GetTick();
    do {
        if (HAL_I2C_IsDeviceReady(dev->hi2c, (uint16_t)(dev->addr7 << 1), 1U,
                                  AD5144_I2C_TIMEOUT_MS) == HAL_OK) {
            return HAL_OK;
        }
        HAL_Delay(1U);
    } while ((HAL_GetTick() - t0) < AD5144_EEPROM_TIMEOUT_MS);

    return HAL_TIMEOUT;
}

HAL_StatusTypeDef AD5144_StoreSafePowerUp(AD5144_Handle_t *dev)
{
    HAL_StatusTypeDef status = HAL_OK;
    AD5144_Channel_t channels[4] = { AD5144_CH_A, AD5144_CH_B, AD5144_CH_C, AD5144_CH_D };

    for (uint8_t i = 0; i < 4U; i++) {
        HAL_StatusTypeDef s = AD5144_StoreEepromCode(dev, channels[i], (uint8_t)AD5144_CODE_MIN_GAIN);
        if (s != HAL_OK) {
            status = s;
        }
    }
    return status;
}
