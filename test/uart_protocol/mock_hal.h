/**
 ******************************************************************************
 * @file    mock_hal.h
 * @brief   Sustituto minimo de "stm32l4xx_hal.h" para el arnes de pruebas de
 *          escritorio de uart_protocol.
 *
 * uart_protocol.c arrastra session_fsm.h, que a su vez incluye ad5144.h,
 * dac_waveform.h y culture_selector.h (para tipos y constantes: DacBank_t,
 * DAC_FREQ_MIN/MAX_HZ, SESSION_*...). Este mock solo aporta los tipos HAL
 * que esas cabeceras necesitan para compilar. La logica de session_fsm NO
 * se compila: sus funciones son mocks (ver mocks.c). dac_waveform tampoco:
 * uart_protocol.c no lo llama directamente.
 ******************************************************************************
 */
#ifndef TEST_MOCK_HAL_H_
#define TEST_MOCK_HAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/* Handles opacos: solo se usan como tipos de puntero en las cabeceras. */
typedef struct { uint32_t dummy; } I2C_HandleTypeDef;
typedef struct { uint32_t dummy; } DAC_HandleTypeDef;
typedef struct { uint32_t dummy; } TIM_HandleTypeDef;
typedef struct { uint32_t dummy; } RTC_HandleTypeDef;

#endif /* TEST_MOCK_HAL_H_ */
