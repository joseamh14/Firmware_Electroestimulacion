/**
 ******************************************************************************
 * @file    usbd_cdc_if.h  (shim de escritorio)
 * @brief   Sustituto de la interfaz USB CDC generada por CubeMX para el
 *          arnes de pruebas de uart_protocol.
 *
 * Declara SOLO lo que toca uart_protocol.c: el tipo del handle USB con su
 * campo pClassData, el handle CDC con TxState, la constante USBD_BUSY y el
 * prototipo de CDC_Transmit_FS. La implementacion (espia de transmision)
 * esta en mocks.c.
 ******************************************************************************
 */
#ifndef TEST_USBD_CDC_IF_H_
#define TEST_USBD_CDC_IF_H_

#include <stdint.h>
#include <stdbool.h>

/* Subconjunto de USBD_StatusTypeDef (usbd_def.h): uart_protocol.c compara
 * el retorno de CDC_Transmit_FS contra USBD_BUSY. */
#define USBD_OK    (0U)
#define USBD_BUSY  (1U)
#define USBD_FAIL  (3U)

typedef struct {
    void *pClassData;
} USBD_HandleTypeDef;

typedef struct {
    uint32_t TxState;
    uint32_t RxState;
} USBD_CDC_HandleTypeDef;

/* Definido en mocks.c (uart_protocol.c lo declara 'extern' por su cuenta). */
extern USBD_HandleTypeDef hUsbDeviceFS;

uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len);

/* ===================================================================== *
 *  Controles del banco de pruebas (no forman parte de la interfaz CDC)
 * ===================================================================== */
typedef struct {
    int      calls;          /* nº de llamadas a CDC_Transmit_FS           */
    uint32_t len;            /* bytes acumulados                          */
    char     buf[2048];      /* concatenacion de todo lo transmitido      */
} TxSpy_t;

extern TxSpy_t g_tx;
void TxSpy_Reset(void);

/* true  -> pila USB "configurada": CDC_Transmit_FS graba en el espia.
 * false -> pClassData = NULL: uart_protocol.c descarta la respuesta.     */
void MockUsb_SetConfigured(bool configured);

#endif /* TEST_USBD_CDC_IF_H_ */
