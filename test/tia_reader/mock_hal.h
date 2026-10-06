/**
 ******************************************************************************
 * @file    mock_hal.h
 * @brief   Sustituto minimo de "stm32l4xx_hal.h" para el arnes de
 *          tia_reader y current_log.
 *
 * tia_reader.c se compila con TIA_READER_HOST_TEST: las funciones del
 * nucleo (espera en us, seccion critica, seleccion del disparo del ADC) y
 * la HAL del ADC las aportan los espias de mocks.c. El arnes completa las
 * rafagas a mano llamando a TiaReader_OnBurstComplete(), como haria la
 * interrupcion del DMA.
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

typedef struct { uint32_t ExternalTrigConv; } ADC_InitTypeDef;
typedef struct { void *Instance; ADC_InitTypeDef Init; } ADC_HandleTypeDef;
typedef struct { uint32_t dummy; } DAC_HandleTypeDef;
typedef struct { uint32_t dummy; } TIM_HandleTypeDef;
typedef struct { uint32_t dummy; } GPIO_TypeDef;

#define ADC_SINGLE_ENDED  (0x7FU)

HAL_StatusTypeDef HAL_ADC_Start_DMA(ADC_HandleTypeDef *hadc, uint32_t *pData, uint32_t Length);
HAL_StatusTypeDef HAL_ADC_Stop_DMA(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *hadc, uint32_t SingleDiff);
void              HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc);
uint32_t          HAL_GetTick(void);

/* --- Espias y controles --- */
#define SPY_MAX_ARMS  64

typedef struct {
    uint32_t tick;                    /* reloj falso de HAL_GetTick          */
    uint32_t freq[2];                 /* frecuencia de cada banco (DAC)      */
    HAL_StatusTypeDef start_ret;      /* retorno forzado de Start_DMA        */

    int      starts;                  /* llamadas a HAL_ADC_Start_DMA        */
    int      stops;                   /* llamadas a HAL_ADC_Stop_DMA         */
    int      calibs;
    uint16_t *buf;                    /* buffer que recibio el ultimo Start  */
    uint32_t len;                     /* longitud del ultimo Start           */
    int      trig_bank[SPY_MAX_ARMS]; /* banco del disparo en cada armado    */
    uint32_t arm_len[SPY_MAX_ARMS];
    int      arm_ch[SPY_MAX_ARMS];    /* canal del mux en cada armado        */

    int      mux_selects;
    int      mux_disables;
    int      mux_ch;                  /* ultimo canal seleccionado (0 = off) */
    int      waits;
    uint32_t last_wait_us;
    int      irq_depth;               /* secciones criticas abiertas         */
} TiaSpy_t;

extern TiaSpy_t g_spy;
void TiaSpy_Reset(void);
void TiaSpy_FillBurst(uint16_t lo, uint16_t hi);   /* rampa lo..hi en el buffer */

#endif /* TEST_MOCK_HAL_H_ */
