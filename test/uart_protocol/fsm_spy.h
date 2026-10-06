/**
 ******************************************************************************
 * @file    fsm_spy.h
 * @brief   Espia de la capa session_fsm para el arnes de uart_protocol.
 *
 * Cada mock de SessionFSM_* registra aqui con que argumentos se le llamo y
 * cuantas veces. 'mutations' agrega todas las llamadas que CAMBIAN estado
 * (Start / setters / Stop) -- NO incluye GetStatus -- para poder afirmar
 * "una entrada invalida no toca la FSM". 'ret' es el valor que devuelven
 * esas cinco funciones (forzar HAL_ERROR simula un fallo de periferico).
 ******************************************************************************
 */
#ifndef TEST_FSM_SPY_H_
#define TEST_FSM_SPY_H_

#include <stdint.h>
#include <stdbool.h>
#include "mock_hal.h"

typedef struct {
    int mutations;          /* Start + setters + Stop (sin GetStatus)     */

    int      setamp_calls;
    uint8_t  setamp_channel;
    uint16_t setamp_mv;

    int      setfreq_calls;
    int      setfreq_bank;   /* DacBank_t como int                        */
    uint32_t setfreq_hz;

    int      setwell_calls;
    uint8_t  setwell_channel;
    bool     setwell_connected;

    int      start_calls;
    uint32_t start_on_s;
    uint32_t start_off_s;
    uint32_t start_duration_s;

    int      stop_calls;
    int      getstatus_calls;
    int      eeprom_calls;
    HAL_StatusTypeDef eeprom_ret;   /* retorno de StoreAmplifierSafeDefault */

    HAL_StatusTypeDef ret;   /* retorno de Start / setters / Stop         */
} FsmSpy_t;

extern FsmSpy_t g_fsm;
void FsmSpy_Reset(void);

/* Registrador de corriente simulado (GET CUR) y flash simulada (SAVE CAL) */
#include "sd_logger.h"
extern SdLogStatus_t     g_log;
extern HAL_StatusTypeDef g_flash_ret;
extern int               g_flash_appends;
void LogSpy_Reset(void);

#endif /* TEST_FSM_SPY_H_ */
