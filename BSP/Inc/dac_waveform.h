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
 * El DAC fija la amplitud de ENTRADA comun del banco: oscila siempre
 * entre DAC_CODE_LOW y DAC_CODE_HIGH, simetrico respecto a VBIAS. La
 * amplitud INDIVIDUAL de cada uno de los 8 canales la fija despues el
 * AD5144 como reostato en la realimentacion de un inversor (ver
 * ad5144.h), que amplifica esa misma onda con ganancia R_AW/2,5 kOhm.
 * Este driver NO conoce ni gestiona la amplitud por canal, solo la
 * frecuencia, la excursion compartida del banco y el aparcado en VBIAS.
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

/* Codigos de 12 bits de la onda (revision de esquematicos de octubre de
 * 2026). La etapa diferencial de ESA (OPA2172 + LT5400-4, 4 x 1 kOhm,
 * relacion 1:1) entrega Dif = VBIAS - DAC_OUT, y VBIAS = VREF/2 sale del
 * mismo REF19xx que alimenta VREF+. Por tanto el codigo que da Dif = 0 es
 * exactamente la mitad de escala, sea cual sea el valor real de VREF
 * (diseno ratiometrico): DAC_CODE_MID = 2048.
 *
 * La excursion +/-DAC_SWING_CODES fija la amplitud de pico de Dif que
 * despues amplifica cada canal (ganancia R_AW/2,5 kOhm, ver ad5144.h):
 *   - techo: con D = 0 y R_AB en +8 % (ganancia 4,37) la salida no debe
 *     pasar de los +/-2,5 V que admiten los terminales del AD5144
 *     -> excursion <= 0,572 V (710 codigos).
 *   - suelo: con R_AB en -8 % (ganancia max. 3,70) aun deben alcanzarse
 *     2 V de pico -> excursion >= 0,540 V (671 codigos).
 * 690 codigos (0,556 V con VREF = 3,3 V) cumple ambos; resolucion por
 * canal ~8,7 mV/paso, por debajo de los 10 mV pedidos.
 * Pendiente de confirmar: si los "0-2 V" del requisito son de pico (lo que
 * aqui se asume) o pico a pico. */
#define DAC_VREF_MV            (3300U)
#define DAC_CODE_MID           (2048U)
#define DAC_SWING_CODES        (690U)
#define DAC_CODE_HIGH          (DAC_CODE_MID + DAC_SWING_CODES)
#define DAC_CODE_LOW           (DAC_CODE_MID - DAC_SWING_CODES)

/* Amplitud de pico de Dif en mV (redondeada) para la conversion
 * mV -> codigo del AD5144: 690 * 3300 / 4096 = 555,9 -> 556 mV. */
#define DAC_DIF_PEAK_MV        ((uint16_t)(((uint32_t)DAC_SWING_CODES * DAC_VREF_MV + 2048U) / 4096U))

/**
 * @brief  Vincula los handles de DAC/timer generados por CubeMX, prepara
 *         el buffer circular de 2 muestras por banco y deja ambos bancos
 *         detenidos (0 Hz) y APARCADOS en DAC_CODE_MID (Dif = 0) hasta que
 *         se pida una frecuencia explicita.
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
 * @brief  Detiene la onda del banco (timer de disparo y DMA) y deja la
 *         salida del DAC APARCADA en DAC_CODE_MID, es decir, Dif = 0 y
 *         salida de los cuatro canales del banco a 0 V sea cual sea el
 *         codigo del AD5144. No se deshabilita el canal: con el DAC
 *         deshabilitado el pin queda en alta impedancia y la etapa
 *         diferencial entregaria una continua de -VBIAS/2 a los pocillos.
 */
HAL_StatusTypeDef DacWaveform_Stop(DacBank_t bank);

uint32_t DacWaveform_GetFrequency(DacBank_t bank);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_DAC_WAVEFORM_H_ */
