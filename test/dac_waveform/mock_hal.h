/**
 ******************************************************************************
 * @file    mock_hal.h
 * @brief   Sustituto minimo de "stm32l4xx_hal.h" para el arnes de pruebas de
 *          escritorio de dac_waveform.
 *
 * dac_waveform.c se compila SIN modificar (se incluye entero en el TU de
 * test para alcanzar la funcion estatica ComputeTimerParams). Este mock solo
 * aporta lo que ese fichero necesita del entorno HAL:
 *
 *   - HAL_StatusTypeDef.
 *   - Handles opacos de DAC y TIM. El handle de TIM expone PSC/ARR/CNT como
 *     campos observables: son el equivalente de escritorio de los registros
 *     TIMx->PSC / TIMx->ARR / TIMx->CNT que escribe el driver real, lo que
 *     permite verificar por el API publico que la aritmetica llega bien al
 *     temporizador.
 *   - Las macros __HAL_TIM_SET_* y las constantes DAC_CHANNEL_* / DAC_ALIGN_*.
 *   - Prototipos de las cuatro funciones HAL de arranque/parada. Sus stubs
 *     no-op estan en mocks.c: la aritmetica de ComputeTimerParams no las usa.
 ******************************************************************************
 */
#ifndef TEST_MOCK_HAL_H_
#define TEST_MOCK_HAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --- Estado de retorno de HAL ----------------------------------------- */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/* --- Handles -----------------------------------------------------------
 * El TIM lleva PSC/ARR/CNT "a pelo" (sin ->Instance) para que las macros
 * __HAL_TIM_SET_* de abajo escriban directamente sobre algo inspeccionable. */
typedef struct {
    uint32_t PSC;
    uint32_t ARR;
    uint32_t CNT;
} TIM_HandleTypeDef;

typedef struct { uint32_t dummy; } DAC_HandleTypeDef;

/* --- Constantes de la HAL del DAC que referencia dac_waveform.c ------- */
#define DAC_CHANNEL_1     (0x00000000U)
#define DAC_CHANNEL_2     (0x00000010U)
#define DAC_ALIGN_12B_R   (0x00000000U)

/* --- Acceso a "registro" del timer (equivalente de escritorio de las
 *     __HAL_TIM_SET_* de stm32l4xx_hal_tim.h) --------------------------- */
#define __HAL_TIM_SET_PRESCALER(__HANDLE__, __VAL__)   ((__HANDLE__)->PSC = (uint32_t)(__VAL__))
#define __HAL_TIM_SET_AUTORELOAD(__HANDLE__, __VAL__)  ((__HANDLE__)->ARR = (uint32_t)(__VAL__))
#define __HAL_TIM_SET_COUNTER(__HANDLE__, __VAL__)     ((__HANDLE__)->CNT = (uint32_t)(__VAL__))

/* --- API de HAL que usa dac_waveform.c (stubs en mocks.c) ------------- */
HAL_StatusTypeDef HAL_TIM_Base_Start(TIM_HandleTypeDef *htim);
HAL_StatusTypeDef HAL_TIM_Base_Stop (TIM_HandleTypeDef *htim);
HAL_StatusTypeDef HAL_DAC_Start_DMA(DAC_HandleTypeDef *hdac, uint32_t Channel,
                                    uint32_t *pData, uint32_t Length, uint32_t Alignment);
HAL_StatusTypeDef HAL_DAC_Stop_DMA (DAC_HandleTypeDef *hdac, uint32_t Channel);

/* ===================================================================== *
 *  Espia de la capa HAL (no forma parte de HAL)
 * ===================================================================== */
typedef struct {
    int tim_start_calls;
    int tim_stop_calls;
    int dac_start_dma_calls;
    int dac_stop_dma_calls;
    HAL_StatusTypeDef dac_start_dma_ret;   /* forzable para probar propagacion de fallo */
} DacHalSpy_t;

extern DacHalSpy_t g_hal;
void DacHalSpy_Reset(void);

#endif /* TEST_MOCK_HAL_H_ */
