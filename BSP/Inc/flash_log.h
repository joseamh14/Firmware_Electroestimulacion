/**
 ******************************************************************************
 * @file    flash_log.h
 * @brief   Registro de solo anexado en la flash interna, tolerante a cortes
 *          de alimentacion (persistencia de sesion y de calibracion).
 *
 * Cada registro se reparte entre DOS sectores de 8 KB en ping-pong. Los
 * registros son de tamano fijo (multiplo de 16 bytes) y llevan cabecera
 * (marca + numero de secuencia) y CRC-32 al final:
 *
 *   [magic 4 B][seq 4 B][payload ...][relleno][crc 4 B]
 *
 * - Escritura: se anexa en el primer hueco borrado del sector activo. Si
 *   esta lleno, se borra el OTRO sector y se escribe alli (seq + 1). Nunca
 *   se borra el sector que contiene el ultimo registro valido antes de que
 *   exista uno mas nuevo, de modo que un corte en cualquier punto deja,
 *   como poco, el registro anterior intacto.
 * - Lectura: se recorren ambos sectores y gana el registro con CRC correcto
 *   y secuencia mas alta.
 *
 * Un registro a medio programar (corte durante la escritura) falla el CRC y
 * se ignora. Si el corte deja una doble palabra con error ECC doble, su
 * lectura dispara la NMI: el manejador (stm32l4xx_it.c) limpia el error y
 * marca FlashLog_EccFault para que este modulo descarte ese registro.
 *
 * La geometria fisica de la flash del STM32L4R5 depende del bit DBANK de
 * los option bytes (doble banco con paginas de 4 KB, o banco unico con
 * paginas de 8 KB). El sector logico de 8 KB se traduce en tiempo de
 * ejecucion a 2 paginas o a 1 pagina, segun corresponda.
 *
 * Mapa de los ultimos 32 KB de la flash de 2 MB (fuera del alcance de la
 * imagen, que ocupa < 100 KB; FlashLog_RegionIsFree() lo comprueba):
 *   0x081F8000  sesion,      sector A
 *   0x081FA000  sesion,      sector B
 *   0x081FC000  calibracion, sector A
 *   0x081FE000  calibracion, sector B
 ******************************************************************************
 */
#ifndef BSP_INC_FLASH_LOG_H_
#define BSP_INC_FLASH_LOG_H_

#include "stm32l4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLASH_LOG_SECTOR_SIZE        (0x2000UL)   /* 8 KB logicos               */
#define FLASH_LOG_REGION_START       (0x081F8000UL)
#define FLASH_LOG_SESSION_SECTOR_A   (0x081F8000UL)
#define FLASH_LOG_SESSION_SECTOR_B   (0x081FA000UL)
#define FLASH_LOG_CAL_SECTOR_A       (0x081FC000UL)
#define FLASH_LOG_CAL_SECTOR_B       (0x081FE000UL)

#define FLASH_LOG_HEADER_BYTES       (8U)         /* magic + seq                */
#define FLASH_LOG_CRC_BYTES          (4U)
#define FLASH_LOG_ALIGN              (16U)        /* alineacion de registro     */
#define FLASH_LOG_MAX_RECORD         (256U)

typedef struct {
    uint32_t sector[2];      /* direccion base de los dos sectores           */
    uint32_t magic;          /* identifica el tipo de registro               */
    uint32_t record_size;    /* bytes por registro, multiplo de 16, <= 256   */
} FlashLog_Config_t;

/* Lo pone a true el manejador de NMI cuando detecta un error ECC doble en
 * la flash; este modulo lo consulta y lo limpia en cada lectura. */
extern volatile bool FlashLog_EccFault;

/** Tamano de registro necesario para un payload de payload_len bytes. */
#define FLASH_LOG_RECORD_SIZE_FOR(payload_len) \
    ((((FLASH_LOG_HEADER_BYTES + (payload_len) + FLASH_LOG_CRC_BYTES) + \
       (FLASH_LOG_ALIGN - 1U)) / FLASH_LOG_ALIGN) * FLASH_LOG_ALIGN)

/**
 * @brief  Lee el payload del registro valido mas reciente.
 * @param  out_seq  Secuencia del registro devuelto (puede ser NULL).
 * @retval HAL_OK si hay un registro valido; HAL_ERROR si no hay ninguno o
 *         los argumentos no son validos.
 */
HAL_StatusTypeDef FlashLog_ReadLatest(const FlashLog_Config_t *cfg, void *payload,
                                      uint32_t payload_len, uint32_t *out_seq);

/**
 * @brief  Anexa un registro nuevo con el payload dado. Puede tardar varias
 *         decenas de ms si toca borrar un sector.
 */
HAL_StatusTypeDef FlashLog_Append(const FlashLog_Config_t *cfg, const void *payload,
                                  uint32_t payload_len);

/**
 * @brief  Comprueba que la imagen enlazada no invade la region reservada.
 *         Lectura y escritura la consultan y fallan (HAL_ERROR) si no.
 */
bool FlashLog_RegionIsFree(void);

/** CRC-32 (polinomio reflejado 0xEDB88320, el de Ethernet/zip). */
uint32_t FlashLog_Crc32(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BSP_INC_FLASH_LOG_H_ */
