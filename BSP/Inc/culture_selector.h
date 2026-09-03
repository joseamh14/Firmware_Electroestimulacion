/**
 ******************************************************************************
 * @file    culture_selector.h
 * @brief   Selector de pocillo (ADG1634, U3/U4) -- interruptor simple.
 *
 * Cada uno de los 8 pocillos de cultivo cuelga de una linea IN del
 * ADG1634 correspondiente (U3 para los pocillos 1-4, U4 para el 5-8) que
 * simplemente conecta o desconecta ese pocillo de la senal de
 * estimulacion ya generada (DAC + AD5144). No tiene relacion con la
 * frecuencia ni con la amplitud -- eso lo resuelven dac_waveform.h y
 * ad5144.h respectivamente. Este driver solo pone esas 8 lineas en
 * alto o bajo.
 *
 * Pines (confirmados en Altium): U3 IN1-4 -> PE9/PE11/PE13/PE14,
 * U4 IN1-4 -> PD12/PD13/PD14/PD15, configurados en CubeMX como salida
 * digital simple (no como PWM -- ver docs/cubemx_config_checklist.md).
 ******************************************************************************
 */
#ifndef BSP_INC_CULTURE_SELECTOR_H_
#define BSP_INC_CULTURE_SELECTOR_H_

#include "stm32l4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pocillo 1..8, en el mismo orden que los canales de amplitud del AD5144
 * (ver AD5144_Channel_t en ad5144.h): 1-4 en el primer chip AD5144/ADG1634,
 * 5-8 en el segundo. */
#define CULTURE_WELL_MIN   (1U)
#define CULTURE_WELL_MAX   (8U)

/**
 * @brief  Conecta o desconecta un pocillo de la senal de estimulacion.
 * @param  well        Numero de pocillo, 1..8.
 * @param  connected   true = conectado a la senal; false = desconectado.
 */
HAL_StatusTypeDef CultureSelector_SetWell(uint8_t well, bool connected);

/**
 * @brief  Desconecta los 8 pocillos. Se usa en el arranque y ante
 *         cualquier condicion de fallo, junto con AD5144_AllChannelsZero,
 *         como parte de la postura de seguridad "todo a 0 V" por defecto.
 */
HAL_StatusTypeDef CultureSelector_AllDisconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_CULTURE_SELECTOR_H_ */
