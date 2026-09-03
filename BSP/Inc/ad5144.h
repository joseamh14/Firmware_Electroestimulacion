/**
 ******************************************************************************
 * @file    ad5144.h
 * @brief   Driver para el potenciometro digital cuadruple AD5144 (I2C).
 *
 * En la placa hay dos instancias (U1 = canales 1-4, U2 = canales 5-8) que
 * fijan la amplitud de cada uno de los ocho canales de estimulacion entre
 * 0 y 1 V, aguas abajo de la etapa diferencial de ESA.
 *
 * Formato del byte de comando (ver datasheet AD5144, tabla de instrucciones):
 *   bits [7:4] = codigo de instruccion. 0001 = escritura al registro RDAC.
 *   bits [3:0] = direccion del canal dentro del chip (0=A, 1=B, 2=C, 3=D).
 *   Segundo byte = valor de 8 bits del wiper (0..255, resolucion 100 kOhm/255).
 ******************************************************************************
 */
#ifndef BSP_INC_AD5144_H_
#define BSP_INC_AD5144_H_

#include "stm32l4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direccion I2C de 7 bits segun el strap del pin ADDR del AD5144.
 * Ajustar segun lo que finalmente quede en el esquematico de cada chip. */
#define AD5144_I2C_ADDR_ADDR_GND   (0x28U) /* 0101000 */
#define AD5144_I2C_ADDR_ADDR_VLOG  (0x2BU) /* 0101011 */

/* Timeout por defecto para las transacciones I2C (ms) */
#define AD5144_I2C_TIMEOUT_MS      (50U)

typedef enum {
    AD5144_CH_A = 0x00U,
    AD5144_CH_B = 0x01U,
    AD5144_CH_C = 0x02U,
    AD5144_CH_D = 0x03U
} AD5144_Channel_t;

typedef struct {
    I2C_HandleTypeDef *hi2c;   /* Bus I2C al que esta colgado el chip      */
    uint8_t            addr7; /* Direccion I2C de 7 bits (sin desplazar)  */
    uint8_t            shadow[4]; /* Ultimo valor escrito por canal, para
                                      poder leer el estado sin transaccion
                                      I2C (util para telemetria/logging)   */
} AD5144_Handle_t;

/**
 * @brief  Inicializa el descriptor logico de un AD5144. No escribe nada
 *         al bus; solo prepara la estructura.
 */
HAL_StatusTypeDef AD5144_Init(AD5144_Handle_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7);

/**
 * @brief  Escribe un codigo crudo (0..255) en el wiper de un canal.
 */
HAL_StatusTypeDef AD5144_SetChannelRaw(AD5144_Handle_t *dev, AD5144_Channel_t ch, uint8_t code);

/**
 * @brief  Escribe la amplitud deseada expresada en milivoltios (0..1000),
 *         convirtiendo internamente al codigo de 8 bits del wiper segun
 *         la ganancia de la etapa ESA. full_scale_mv es la tension que
 *         corresponde al codigo 255 con el wiper a maxima resistencia,
 *         calculada en el Anexo de calculos a partir de RN1/RN2 y las
 *         resistencias de realimentacion de IC2/IC3.
 */
HAL_StatusTypeDef AD5144_SetChannelMillivolts(AD5144_Handle_t *dev, AD5144_Channel_t ch,
                                               uint16_t target_mv, uint16_t full_scale_mv);

/**
 * @brief  Pone a cero (wiper a 0) los cuatro canales del chip. Se usa en el
 *         arranque y ante cualquier condicion de fallo, para garantizar que
 *         el sistema arranca siempre en 0 V por seguridad del cultivo.
 */
HAL_StatusTypeDef AD5144_AllChannelsZero(AD5144_Handle_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_AD5144_H_ */
