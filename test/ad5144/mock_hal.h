/**
 ******************************************************************************
 * @file    mock_hal.h
 * @brief   Sustituto minimo de "stm32l4xx_hal.h" para el arnes de pruebas de
 *          escritorio de ad5144.
 *
 * ad5144.c se compila SIN modificar (se incluye entero en el TU de test para
 * alcanzar la funcion estatica BuildCommandByte). Este mock solo aporta lo
 * que ese fichero necesita del entorno HAL:
 *
 *   - HAL_StatusTypeDef.
 *   - Handle opaco de I2C (solo se usa como tipo de puntero).
 *   - Prototipos de HAL_I2C_IsDeviceReady y HAL_I2C_Master_Transmit. Sus
 *     mocks-espia estan en mocks.c: registran con que argumentos se les
 *     llamo (direccion, bytes de la trama, tamano, timeout) y devuelven un
 *     estado configurable por llamada, para poder afirmar "el comando X
 *     genera exactamente la trama Z" y "un fallo del bus no toca el shadow".
 ******************************************************************************
 */
#ifndef TEST_MOCK_HAL_H_
#define TEST_MOCK_HAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --- Estado de retorno de HAL ---------------------------------------- */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/* --- Handle opaco de I2C -------------------------------------------------
 * ad5144.c solo lo desreferencia para comparar con NULL y lo pasa como
 * puntero a las funciones HAL: un struct dummy es suficiente. */
typedef struct { uint32_t dummy; } I2C_HandleTypeDef;

/* --- API de HAL que usa ad5144.c (mocks-espia en mocks.c) ----------- */
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                        uint32_t Trials, uint32_t Timeout);

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
                                          uint8_t *pData, uint16_t Size, uint32_t Timeout);

/* Base de tiempo (la usa el sondeo por ACK tras escribir la EEPROM). El
 * reloj falso solo avanza con HAL_Delay. */
uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t Delay);

/* ===================================================================== *
 *  Espia de la capa HAL de I2C (no forma parte de HAL)
 * ===================================================================== */
#define AD5144_SPY_MAX_TX  16   /* historial de transacciones capturadas */

typedef struct {
    /* --- HAL_I2C_IsDeviceReady --- */
    int               ready_calls;
    uint16_t          ready_addr;      /* ultima direccion (ya << 1)      */
    uint32_t          ready_trials;    /* ultimo Trials                   */
    uint32_t          ready_timeout;   /* ultimo Timeout                  */
    HAL_StatusTypeDef ready_ret;       /* retorno forzado                 */
    int               ready_busy_left; /* n primeras llamadas -> HAL_ERROR
                                          (chip ocupado programando EEPROM) */
    uint32_t          tick_ms;         /* reloj falso de HAL_GetTick       */
    int               delay_calls;

    /* --- HAL_I2C_Master_Transmit --- */
    int               tx_calls;
    uint16_t          tx_addr[AD5144_SPY_MAX_TX];    /* direccion (ya << 1) */
    uint8_t           tx_b0[AD5144_SPY_MAX_TX];      /* byte 0 (comando)   */
    uint8_t           tx_b1[AD5144_SPY_MAX_TX];      /* byte 1 (wiper)     */
    uint16_t          tx_size[AD5144_SPY_MAX_TX];    /* Size de la trama   */
    uint32_t          tx_timeout[AD5144_SPY_MAX_TX]; /* Timeout            */
    HAL_StatusTypeDef tx_ret;                        /* retorno por defecto */
    int               tx_ret_seq[AD5144_SPY_MAX_TX]; /* -1 = usar tx_ret;
                                                        si no, se fuerza ese
                                                        valor en esa llamada */
} Ad5144HalSpy_t;

extern Ad5144HalSpy_t g_spy;

void Ad5144Spy_Reset(void);

#endif /* TEST_MOCK_HAL_H_ */
