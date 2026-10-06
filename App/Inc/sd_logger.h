/**
 ******************************************************************************
 * @file    sd_logger.h
 * @brief   Medida de corriente por tramos de estimulacion y registro binario
 *          en la microSD (FatFs sobre SDMMC1).
 *
 * - Al entrar la sesion en STIM_ON arranca la lectura (tia_reader.h) de los
 *   8 canales y abre un fichero nuevo BNNNN.BIN en la raiz de la tarjeta
 *   (primer numero libre). Al salir de STIM_ON (REST, IDLE) para la lectura
 *   y cierra el fichero: un fichero por tramo de estimulacion. En reposo no
 *   se registra.
 * - Cada segundo (reloj del sistema, HAL_GetTick) cierra una ventana,
 *   calcula el valor de cada canal (current_log.h) y anexa un registro de
 *   36 bytes. Un tramo de 6 h ocupa 36 x 21 600 = 777 600 bytes.
 * - Para no competir con las interrupciones del DMA del ADC mientras se
 *   escribe en la tarjeta (SDMMC en modo sondeo), la lectura se suspende
 *   durante la escritura (unos ms por segundo).
 * - f_sync cada SD_LOGGER_SYNC_EVERY registros: ante un corte se pierden
 *   como mucho esos segundos del fichero.
 * - Sin tarjeta, o con un error de escritura, se sigue midiendo (comando
 *   GET CUR) y el registro queda desactivado hasta el siguiente tramo.
 ******************************************************************************
 */
#ifndef APP_INC_SD_LOGGER_H_
#define APP_INC_SD_LOGGER_H_

#include "current_log.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_LOGGER_PERIOD_MS     (1000UL)
#define SD_LOGGER_SYNC_EVERY    (10U)
#define SD_LOGGER_MAX_FILES     (9999U)

typedef enum {
    SD_LOG_OFF = 0,     /* fuera de STIM_ON                                 */
    SD_LOG_ACTIVE,      /* midiendo y escribiendo en la tarjeta              */
    SD_LOG_NO_CARD      /* midiendo, sin tarjeta o tras un error de escritura */
} SdLogState_t;

typedef struct {
    SdLogState_t state;
    char         file[13];                 /* "B0001.BIN" o ""              */
    uint32_t     records;                  /* registros escritos en el tramo */
    bool         have_values;              /* hay al menos una ventana       */
    float        last_ua[CURRENT_LOG_NUM_CH];
} SdLogStatus_t;

void SdLogger_Init(void);

/** Llamar desde el bucle principal. */
void SdLogger_Task(void);

void SdLogger_GetStatus(SdLogStatus_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_INC_SD_LOGGER_H_ */
