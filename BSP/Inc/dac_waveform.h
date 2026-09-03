/**
 ******************************************************************************
 * @file    dac_waveform.h
 * @brief   Generacion de la onda cuadrada de estimulacion por DAC + DMA.
 *
 * El DAC1 del STM32L4R5 (un unico periferico, dos canales: DAC1_OUT1 para
 * el banco A -canales de cultivo 1-4-, DAC1_OUT2 para el banco B -canales
 * 5-8-) genera directamente la onda cuadrada de cada banco. Es el uso
 * estandar de un DAC con disparo por timer y volcado circular por DMA,
 * el mismo mecanismo que ya validaba el firmware de pruebas antiguo
 * (DAC_RTC_L4), aqui generalizado a frecuencia configurable en lugar de
 * fija a 100 Hz.
 *
 * El DAC fija el TECHO de amplitud: siempre oscila a fondo de escala
 * (DAC_CODE_LOW <-> DAC_CODE_HIGH). La amplitud INDIVIDUAL de cada uno de
 * los 8 canales la ajusta despues el AD5144 (ver ad5144.h), que atenua
 * esta misma onda ya oscilante -- un divisor resistivo no distingue entre
 * continua y alterna, asi que atenua igual de bien una onda cuadrada que
 * un nivel fijo. Este driver NO conoce ni gestiona la amplitud por canal,
 * solo la frecuencia y el techo compartido del banco.
 *
 * TIM1 dispara la conversion del banco A, TIM8 dispara la del banco B
 * (misma pareja de timers que ya usaba el prototipo antiguo, en modo
 * basico de disparo por TRGO, sin canales de PWM).
 *
 * LIMITACION FISICA CONOCIDA Y ACEPTADA (ver capitulo de Requisitos de
 * diseno de la memoria): el tiempo de asentamiento del DAC del STM32L4
 * (~1.6-4 us segun modo/carga) hace que su uso limpio este en torno a los
 * 100 kHz, que es precisamente la frecuencia mas usada segun el
 * investigador. El limite superior de 300 kHz se admite como caso
 * extremo poco frecuente; a esa frecuencia la onda puede salir mas
 * redondeada que cuadrada.
 ******************************************************************************
 */
#ifndef BSP_INC_DAC_WAVEFORM_H_
#define BSP_INC_DAC_WAVEFORM_H_

#include "stm32l4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DAC_BANK_A = 0, /* canales 1-4, DAC1_OUT1, disparo TIM1 */
    DAC_BANK_B = 1  /* canales 5-8, DAC1_OUT2, disparo TIM8 */
} DacBank_t;

/* Reloj efectivo de TIM1/TIM8 con el arbol de reloj de referencia
 * (ver docs/cubemx_config_checklist.md) -- ambos quedan a 120 MHz. */
#define DAC_TRIGGER_CLOCK_HZ   (120000000UL)

#define DAC_FREQ_MIN_HZ        (1UL)
#define DAC_FREQ_MAX_HZ        (300000UL)

/* Codigos de 12 bits que definen el techo de amplitud (fondo de escala
 * compartido por el banco). Punto de partida: los valores que ya usaba
 * el prototipo antiguo (empiricamente probados en continua), pendientes
 * de recalibrar contra la placa real una vez montada -- dependen de la
 * ganancia exacta de la etapa diferencial de ESA y de VBIAS. */
#define DAC_CODE_HIGH          (2630U)
#define DAC_CODE_LOW           (1430U)

/**
 * @brief  Vincula los handles de DAC/timer generados por CubeMX, prepara
 *         el buffer circular de 2 muestras por banco y deja ambos bancos
 *         detenidos (0 Hz) hasta que se pida una frecuencia explicita.
 */
HAL_StatusTypeDef DacWaveform_Init(DAC_HandleTypeDef *hdac,
                                    TIM_HandleTypeDef *htim_bank_a,
                                    TIM_HandleTypeDef *htim_bank_b);

/**
 * @brief  Arranca (o reconfigura en caliente) la onda cuadrada de un banco
 *         a la frecuencia pedida. freq_hz = 0 detiene el banco.
 *         Fuera de [DAC_FREQ_MIN_HZ, DAC_FREQ_MAX_HZ] devuelve HAL_ERROR
 *         sin modificar el estado anterior.
 */
HAL_StatusTypeDef DacWaveform_SetFrequency(DacBank_t bank, uint32_t freq_hz);

/**
 * @brief  Detiene el banco (DAC y timer de disparo). Medida de defensa en
 *         profundidad: la seguridad primaria de "canal a 0 V" la da el
 *         AD5144 a codigo 0, no este detenimiento -- pero parar el DAC
 *         cuando el banco entero esta inactivo ahorra consumo.
 */
HAL_StatusTypeDef DacWaveform_Stop(DacBank_t bank);

uint32_t DacWaveform_GetFrequency(DacBank_t bank);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_DAC_WAVEFORM_H_ */
