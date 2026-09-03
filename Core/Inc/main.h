/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define A0_MUX_Pin GPIO_PIN_0
#define A0_MUX_GPIO_Port GPIOB
#define A1_MUX_Pin GPIO_PIN_1
#define A1_MUX_GPIO_Port GPIOB
#define A2_MUX_Pin GPIO_PIN_2
#define A2_MUX_GPIO_Port GPIOB
#define Cultivo1_Pin GPIO_PIN_9
#define Cultivo1_GPIO_Port GPIOE
#define Cultivo2_Pin GPIO_PIN_11
#define Cultivo2_GPIO_Port GPIOE
#define Cultivo3_Pin GPIO_PIN_13
#define Cultivo3_GPIO_Port GPIOE
#define Cultivo4_Pin GPIO_PIN_14
#define Cultivo4_GPIO_Port GPIOE
#define EN_MUX_Pin GPIO_PIN_10
#define EN_MUX_GPIO_Port GPIOB
#define Cultivo5_Pin GPIO_PIN_12
#define Cultivo5_GPIO_Port GPIOD
#define Cultivo6_Pin GPIO_PIN_13
#define Cultivo6_GPIO_Port GPIOD
#define Cultivo7_Pin GPIO_PIN_14
#define Cultivo7_GPIO_Port GPIOD
#define Cultivo8_Pin GPIO_PIN_15
#define Cultivo8_GPIO_Port GPIOD

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
