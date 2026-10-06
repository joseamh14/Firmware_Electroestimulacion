/**
 ******************************************************************************
 * @file    mock_hal.h
 * @brief   Sustituto minimo de "stm32l4xx_hal.h" para el arnes de flash_log.
 *
 * flash_log.c se compila con FLASH_LOG_HOST_TEST: en ese modo no usa la HAL
 * de flash, sino cuatro funciones de puerto (FlashPort_*) que aporta
 * mocks.c sobre un array en RAM que imita los ultimos 32 KB de la flash.
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

/* --- Control del simulador de flash (mocks.c) --- */
void     SimFlash_Reset(void);              /* todo borrado, sin fallos      */
void     SimFlash_PowerCutAfter(int ops);   /* corte en la operacion n+1     */
void     SimFlash_Reboot(void);             /* vuelve la alimentacion        */
void     SimFlash_CutClean(bool clean);     /* corte entre programaciones    */
bool     SimFlash_IsDown(void);
void     SimFlash_SetImageEnd(uint32_t addr);
void     SimFlash_CorruptDw(uint32_t addr); /* doble palabra con ECC doble   */
int      SimFlash_EraseCount(uint32_t sector_addr);
int      SimFlash_ProgramCount(void);
uint8_t *SimFlash_Raw(uint32_t addr);       /* acceso directo para el arnes  */

#endif /* TEST_MOCK_HAL_H_ */
