/**
 ******************************************************************************
 * @file    tia_mux.h
 * @brief   Selector de lectura TIA (MAX4581, U7 en SUP) -- multiplexor 8:1.
 *
 * Lleva al ADC (ADC_IN = PC0, tras la etapa OPA320) la salida OUT_TIAn del
 * transimpedancia del pocillo n. En la revision de esquematicos de octubre
 * de 2026 el ADG1208 se sustituyo por un MAX4581, y eso cambia la polaridad
 * del pin de habilitacion:
 *
 *   ENABLE = H -> todos los interruptores abiertos (tabla de verdad del
 *                 fabricante); ENABLE = L -> X conectado a X[C:B:A].
 *
 * Es decir, EN_TIA (PB10) es ACTIVO A NIVEL BAJO; el ADG1208 anterior era
 * activo a nivel alto. Esta polaridad queda encapsulada aqui para que el
 * resto del firmware no dependa de ella.
 *
 * Pines (N_micro.png): A0_TIA = PB0 (A), A1_TIA = PB1 (B), A2_TIA = PB2 (C),
 * EN_TIA = PB10. Mapa de canales (N_SUP.png): OUT_TIAn -> X(n-1).
 * En el .ioc los rotulos siguen siendo A0_MUX..A2_MUX / EN_MUX; este driver
 * usa el puerto y numero de pin directamente para no depender del rotulo.
 ******************************************************************************
 */
#ifndef BSP_INC_TIA_MUX_H_
#define BSP_INC_TIA_MUX_H_

#include "stm32l4xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIA_MUX_CH_MIN        (1U)
#define TIA_MUX_CH_MAX        (8U)

#define TIA_MUX_PORT          GPIOB
#define TIA_MUX_PIN_A0        GPIO_PIN_0
#define TIA_MUX_PIN_A1        GPIO_PIN_1
#define TIA_MUX_PIN_A2        GPIO_PIN_2
#define TIA_MUX_PIN_EN        GPIO_PIN_10

/* Nivel del pin EN_TIA para cada estado (MAX4581: ENABLE activo a nivel bajo) */
#define TIA_MUX_EN_LEVEL_ON   GPIO_PIN_RESET
#define TIA_MUX_EN_LEVEL_OFF  GPIO_PIN_SET

/**
 * @brief  Abre todos los interruptores del MAX4581 (EN_TIA = H). Estado de
 *         reposo; llamar en el arranque justo despues de MX_GPIO_Init.
 */
void TiaMux_Disable(void);

/**
 * @brief  Selecciona el canal de lectura 1..8 y habilita el multiplexor.
 *         Las tres lineas de direccion se escriben con un unico acceso a
 *         BSRR para que cambien a la vez.
 * @retval HAL_ERROR si el canal esta fuera de 1..8 (no se toca nada).
 */
HAL_StatusTypeDef TiaMux_Select(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_TIA_MUX_H_ */
