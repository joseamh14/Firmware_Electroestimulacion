/**
 ******************************************************************************
 * @file    tia_reader.h
 * @brief   Medida de la corriente de estimulacion de cada pocillo: lectura
 *          por rafagas del ADC1, sincronizada con la onda del DAC.
 *
 * Cadena: TIA de cada pocillo -> MAX4581 (tia_mux.h) -> etapa OPA320 ->
 * ADC_IN (PC0, ADC1_IN1). La conversion a uA la hace current_calc.h.
 *
 * FUNCIONAMIENTO
 * --------------
 * - Rotacion de canales 1, 2, ..., 8 (apartado 7.12 de la memoria de la
 *   etapa analogica), un canal por medida. Cada medida se sincroniza con el
 *   timer de su banco (TIM1_TRGO para 1-4, TIM8_TRGO para 5-8): el ADC esta
 *   en modo continuo con disparo externo, y la rafaga arranca exactamente
 *   en un flanco de la onda. Como las medidas son secuenciales, nunca hay
 *   dos bancos pidiendo el ADC a la vez.
 * - El MAX4581 cambia de canal en cuanto termina la medida anterior y se
 *   espera TIA_READER_SETTLE_US antes de armar la siguiente rafaga, de modo
 *   que el multiplexor ya esta asentado cuando llega el flanco. Conmutar el
 *   MAX4581 no afecta a la estimulacion: cada pocillo tiene su propio TIA.
 * - Forma de onda: pulso bifasico con reposo (ver dac_waveform.h), de cuatro
 *   disparos del timer por periodo: fase positiva, fase negativa y dos
 *   disparos de reposo en VBIAS.
 * - Modo PERIODO (frecuencias medias y altas): una sola rafaga que cubre un
 *   periodo completo de la onda desde el flanco, de modo que contiene las
 *   dos fases y el reposo. Se usa cuando el periodo cabe en el buffer.
 * - Modo FLANCOS (frecuencias bajas): TIA_READER_EDGE_BURSTS (4) rafagas
 *   cortas, una en cada uno de los cuatro disparos consecutivos del
 *   periodo, de modo que se cubren las dos fases y el reposo. Entre dos
 *   disparos hay T/4 (>= ~95 us en el limite del modo), tiempo de sobra
 *   para rearmar el ADC entre rafagas.
 * - De cada medida se guardan el maximo (pico de la fase positiva) y el
 *   minimo (pico de la fase negativa) en codigos de ADC, y se acumulan por
 *   canal hasta que el registrador recoge la ventana (TiaReader_TakeWindow).
 *   No hace falta saber que flanco es cual: el maximo de un periodo es el
 *   pico positivo y el minimo, el negativo (el reposo queda en VBIAS, entre
 *   ambos). El valor de corriente que se registra es (I+ - I-)/2, calculado
 *   en current_calc.
 * - Un banco detenido (frecuencia 0) o un canal fuera de la mascara se
 *   saltan. Si una rafaga no llega en el tiempo esperado (timer parado a
 *   mitad, etc.), TiaReader_Task la aborta y pasa al siguiente canal.
 *
 * Frecuencia de muestreo: reloj del ADC 80 MHz (PLLSAI1R), 2,5 + 12,5
 * ciclos por conversion -> 5,33 Msps. No verificado contra el datasheet que
 * 80 MHz sea el maximo admitido por el ADC del STM32L4R5.
 *
 * Requisitos de CubeMX (ver revision de octubre de 2026): ADC1 en modo
 * continuo, disparo externo por TIM1 TRGO (este modulo cambia a TIM8 TRGO
 * en tiempo de ejecucion), overrun con sobrescritura, DMA1 Channel 3 en modo
 * Normal, media palabra, con su interrupcion habilitada.
 ******************************************************************************
 */
#ifndef BSP_INC_TIA_READER_H_
#define BSP_INC_TIA_READER_H_

#include "stm32l4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIA_READER_NUM_CH          (8U)
#define TIA_READER_ADC_CLOCK_HZ    (80000000UL)
#define TIA_READER_ADC_CYCLES      (15UL)      /* 2,5 muestreo + 12,5 conversion */
#define TIA_READER_FS_HZ           (TIA_READER_ADC_CLOCK_HZ / TIA_READER_ADC_CYCLES)
#define TIA_READER_BUF_SAMPLES     (2048U)     /* buffer de rafaga (4 KB)        */
#define TIA_READER_EDGE_SAMPLES    (64U)       /* rafaga por flanco, modo flancos   */
#define TIA_READER_EDGE_BURSTS     (4U)        /* rafagas por periodo, modo flancos */
#define TIA_READER_PERIOD_MARGIN   (8U)        /* muestras extra en modo periodo */
#define TIA_READER_SETTLE_US       (2U)        /* asentamiento MAX4581 + OPA320  */
#define TIA_READER_TIMEOUT_EXTRA_MS (20U)      /* margen sobre 2 periodos        */

typedef enum {
    TIA_PLAN_SKIP = 0,      /* banco parado: no se mide                       */
    TIA_PLAN_PERIOD,        /* una rafaga de un periodo completo              */
    TIA_PLAN_EDGES          /* TIA_READER_EDGE_BURSTS rafagas cortas en
                               disparos consecutivos (un periodo completo) */
} TiaPlanMode_t;

typedef struct {
    TiaPlanMode_t mode;
    uint16_t      samples;  /* muestras por rafaga                            */
} TiaPlan_t;

/* Acumulado de una ventana (tipicamente 1 s) por canal 1..8 (indice 0..7). */
typedef struct {
    uint32_t sum_max[TIA_READER_NUM_CH];   /* suma de picos positivos (codigos) */
    uint32_t sum_min[TIA_READER_NUM_CH];   /* suma de picos negativos (codigos) */
    uint16_t count[TIA_READER_NUM_CH];     /* medidas validas en la ventana     */
} TiaWindow_t;

/**
 * @brief  Decide como medir un canal segun la frecuencia de su banco.
 *         Funcion pura (sin perifericos).
 */
TiaPlan_t TiaReader_PlanFor(uint32_t freq_hz);

/**
 * @brief  Siguiente canal de la rotacion 1,2,...,8 despues de
 *         'current' (0 = empezar), saltando los que no esten en la mascara
 *         (bit i = canal i+1). Devuelve 0 si la mascara esta vacia.
 *         Funcion pura.
 */
uint8_t TiaReader_NextChannel(uint8_t current, uint8_t mask);

/**
 * @brief  Vincula los handles generados por CubeMX y calibra el ADC. Deja
 *         la lectura parada y el MAX4581 deshabilitado.
 */
HAL_StatusTypeDef TiaReader_Init(ADC_HandleTypeDef *hadc);

/**
 * @brief  Empieza la rotacion sobre los canales de la mascara y borra el
 *         acumulado.
 */
void TiaReader_Start(uint8_t channel_mask);

/** @brief Para la lectura y deshabilita el MAX4581. */
void TiaReader_Stop(void);

/**
 * @brief  Suspende temporalmente la lectura (sin perder el acumulado) para
 *         que otra tarea larga (escritura en la microSD) no compita con las
 *         interrupciones del DMA. TiaReader_Resume la reanuda.
 */
void TiaReader_Pause(void);
void TiaReader_Resume(void);

bool TiaReader_IsRunning(void);

/**
 * @brief  Copia el acumulado de la ventana en curso y lo pone a cero, de
 *         forma atomica respecto a la interrupcion.
 */
void TiaReader_TakeWindow(TiaWindow_t *out);

/**
 * @brief  Supervision desde el bucle principal: aborta una rafaga que no
 *         llega y rearma el siguiente canal.
 */
void TiaReader_Task(void);

/**
 * @brief  Llamar desde HAL_ADC_ConvCpltCallback cuando hadc es el del
 *         lector (lo hace el propio modulo: define el callback).
 */
void TiaReader_OnBurstComplete(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_TIA_READER_H_ */
