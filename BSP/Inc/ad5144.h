/**
 ******************************************************************************
 * @file    ad5144.h
 * @brief   Driver para el potenciometro digital cuadruple AD5144 (I2C).
 *
 * En la placa hay dos instancias (U1 = canales 1-4, U2 = canales 5-8),
 * referencia AD5144ABRUZ10 (R_AB nominal = 10 kOhm, 256 posiciones).
 *
 * Topologia en ESA (revision de esquematicos de octubre de 2026): cada
 * canal es un amplificador inversor (OPA4172, IC2/IC3) con resistencia de
 * entrada de 2,5 kOhm desde la senal diferencial Dif1/Dif2 y el AD5144 en
 * la realimentacion como REOSTATO: terminal A a la entrada inversora,
 * terminales W y B unidos a la salida. La resistencia efectiva es R_AW:
 *
 *     R_AW(D) = (256 - D)/256 * R_AB + R_W         (datasheet, modo reostato)
 *     |OUT|   = |Dif| * R_AW(D) / 2,5 kOhm
 *
 * Consecuencias para el firmware:
 *   - La relacion codigo -> amplitud es INVERSA: D = 0 da la ganancia
 *     maxima (~4) y D = 255 la minima (~0,07). No existe codigo que dé 0 V;
 *     el 0 V real lo dan el DAC aparcado en VBIAS (dac_waveform) y el
 *     pocillo conmutado a GND (culture_selector).
 *   - La posicion segura del chip es D = 255 (AD5144_CODE_MIN_GAIN), no 0.
 *   - El AD5144 va alimentado a +/-2,5 V: sus terminales no pueden salir de
 *     ese rango. La excursion del DAC (DAC_SWING_CODES en dac_waveform.h)
 *     se elige para que ni con D = 0 y R_AB en +8 % se superen.
 *
 * Formato del byte de comando (datasheet AD5124/AD5144/AD5144A, Comando 1):
 *   bits [7:4] = codigo de instruccion. 0001 = escritura al registro RDAC.
 *   bits [3:0] = direccion del canal dentro del chip (0=A, 1=B, 2=C, 3=D).
 *   Segundo byte = valor de 8 bits del wiper (0..255).
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

/* Direccion I2C de 7 bits segun el strap del pin ADDR (encapsulado TSSOP-20,
 * tabla 15 de la datasheet): ADDR = VLOGIC -> 0101000, ADDR = NC -> 0101010,
 * ADDR = GND -> 0101011. NC no esta disponible en modo bipolar (VSS < 0 V),
 * que es el caso de esta placa (VSS = -2,5 V).
 * En la placa: U1 (canales 1-4) ADDR = 2V5 = VLOGIC; U2 (canales 5-8)
 * ADDR = GND. */
#define AD5144_I2C_ADDR_ADDR_VLOG  (0x28U) /* 0101000 */
#define AD5144_I2C_ADDR_ADDR_GND   (0x2BU) /* 0101011 */

/* Timeout por defecto para las transacciones I2C (ms) */
#define AD5144_I2C_TIMEOUT_MS      (50U)

/* Parametros de la red resistiva (AD5144ABRUZ10). R_W es el valor tipico
 * de la datasheet (55 ohm min, 125 ohm tip); R_AB tiene una tolerancia de
 * +/-8 %. Ambos pendientes de calibrar por canal en placa real si se exige
 * exactitud absoluta de 10 mV y no solo resolucion. */
#define AD5144_RAB_OHM             (10000L)
#define AD5144_RW_OHM              (125L)
#define AD5144_NUM_STEPS           (256L)

/* Resistencia de entrada del inversor de ESA (R1..R8 = 2K5). */
#define AD5144_RIN_OHM             (2500L)

/* Escritura en EEPROM (Comando 11 de la datasheet): tiempo de programacion
 * 15 ms tip. / 50 ms max.; durante ese tiempo el chip no reconoce su
 * direccion I2C (sondeo por ACK). Resistencia: 100 000 ciclos. */
#define AD5144_EEPROM_TIMEOUT_MS   (60U)

/* Codigos de referencia del wiper. */
#define AD5144_CODE_MAX_GAIN       (0U)    /* R_AW maxima -> ganancia ~4      */
#define AD5144_CODE_MIN_GAIN       (255U)  /* R_AW minima -> posicion segura  */
#define AD5144_CODE_POWERUP        (128U)  /* EEPROM de fabrica: mitad escala */

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
 * @brief  Inicializa el descriptor logico de un AD5144 y comprueba que el
 *         chip responde en el bus. No escribe ningun RDAC. El shadow[] se
 *         deja en AD5144_CODE_POWERUP, que es lo que carga el chip de su
 *         EEPROM al encenderse.
 */
HAL_StatusTypeDef AD5144_Init(AD5144_Handle_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7);

/**
 * @brief  Escribe un codigo crudo (0..255) en el wiper de un canal.
 */
HAL_StatusTypeDef AD5144_SetChannelRaw(AD5144_Handle_t *dev, AD5144_Channel_t ch, uint8_t code);

/**
 * @brief  Calcula el codigo del wiper que da una amplitud de salida dada,
 *         con el modelo de reostato descrito arriba. Funcion pura, sin I2C.
 * @param  target_mv   Amplitud de pico deseada a la salida del canal (mV).
 * @param  in_peak_mv  Amplitud de pico de la senal diferencial Dif a la
 *                     entrada del inversor (mV); la fija la excursion del
 *                     DAC del banco (DAC_DIF_PEAK_MV en dac_waveform.h).
 * @param  out_code    Codigo resultante (0..255).
 * @retval HAL_OK si el objetivo es alcanzable. target_mv por debajo del
 *         minimo alcanzable devuelve AD5144_CODE_MIN_GAIN (posicion
 *         segura). HAL_ERROR si in_peak_mv = 0, out_code es NULL o el
 *         objetivo exige mas ganancia de la disponible (no se recorta en
 *         silencio).
 */
HAL_StatusTypeDef AD5144_CodeForMillivolts(uint16_t target_mv, uint16_t in_peak_mv,
                                           uint8_t *out_code);

/**
 * @brief  Escribe la amplitud de pico deseada (mV) en un canal: calcula el
 *         codigo con AD5144_CodeForMillivolts y lo escribe. Si el objetivo
 *         no es alcanzable devuelve HAL_ERROR sin tocar el chip.
 */
HAL_StatusTypeDef AD5144_SetChannelMillivolts(AD5144_Handle_t *dev, AD5144_Channel_t ch,
                                               uint16_t target_mv, uint16_t in_peak_mv);

/**
 * @brief  Lleva los cuatro canales del chip a la posicion segura
 *         (AD5144_CODE_MIN_GAIN, ganancia minima). Se usa en el arranque y
 *         ante cualquier condicion de fallo. Se mantiene el nombre historico
 *         de la API: con la topologia de reostato actual, "cero" significa
 *         ganancia minima, no codigo 0 (que seria la ganancia MAXIMA).
 */
HAL_StatusTypeDef AD5144_AllChannelsZero(AD5144_Handle_t *dev);

/**
 * @brief  Escribe un codigo directamente en la EEPROM de un canal (valor que
 *         carga el chip al encenderse) y espera, por sondeo de ACK, a que
 *         termine la programacion. No modifica el RDAC en uso.
 */
HAL_StatusTypeDef AD5144_StoreEepromCode(AD5144_Handle_t *dev, AD5144_Channel_t ch, uint8_t code);

/**
 * @brief  Graba AD5144_CODE_MIN_GAIN en la EEPROM de los cuatro canales,
 *         para que al encenderse el chip arranque en ganancia minima en
 *         lugar de a mitad de escala. Operacion de mantenimiento (consume
 *         ciclos de EEPROM): no llamar de forma rutinaria. Sigue con el
 *         resto de canales si uno falla y devuelve el ultimo fallo.
 */
HAL_StatusTypeDef AD5144_StoreSafePowerUp(AD5144_Handle_t *dev);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_AD5144_H_ */
