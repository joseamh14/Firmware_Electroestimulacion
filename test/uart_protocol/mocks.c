/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   Mocks de escritorio para el arnes de pruebas de uart_protocol:
 *          espia de transmision CDC + espia de la capa session_fsm.
 *
 * uart_protocol.c se compila SIN modificar contra estos mocks. Ninguno
 * ejecuta logica real: solo registran con que argumentos se les llamo y
 * devuelven un estado configurable, para poder afirmar "el comando X
 * dispara la llamada Y con los argumentos Z" y "una entrada invalida no
 * llama a nada".
 ******************************************************************************
 */
#include "mock_hal.h"
#include "usbd_cdc_if.h"
#include "session_fsm.h"
#include "fsm_spy.h"

#include <string.h>

/* ===================================================================== *
 *  Espia de transmision USB CDC
 * ===================================================================== */
USBD_HandleTypeDef      hUsbDeviceFS;
static USBD_CDC_HandleTypeDef s_hcdc;

TxSpy_t g_tx;

void TxSpy_Reset(void)
{
    memset(&g_tx, 0, sizeof(g_tx));
}

void MockUsb_SetConfigured(bool configured)
{
    if (configured) {
        s_hcdc.TxState = 0U;                 /* nada en vuelo: sin espera */
        hUsbDeviceFS.pClassData = &s_hcdc;
    } else {
        hUsbDeviceFS.pClassData = NULL;
    }
}

uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len)
{
    g_tx.calls++;
    for (uint16_t i = 0U; i < Len; i++) {
        if (g_tx.len < sizeof(g_tx.buf) - 1U) {
            g_tx.buf[g_tx.len++] = (char)Buf[i];
        }
    }
    g_tx.buf[g_tx.len] = '\0';
    return (uint8_t)USBD_OK;
}

/* ===================================================================== *
 *  Espia de la capa session_fsm
 * ===================================================================== */
FsmSpy_t g_fsm;

void FsmSpy_Reset(void)
{
    memset(&g_fsm, 0, sizeof(g_fsm));
    g_fsm.ret = HAL_OK;
}

HAL_StatusTypeDef SessionFSM_SetChannelMillivolts(uint8_t channel, uint16_t target_mv)
{
    g_fsm.mutations++;
    g_fsm.setamp_calls++;
    g_fsm.setamp_channel = channel;
    g_fsm.setamp_mv = target_mv;
    return g_fsm.ret;
}

HAL_StatusTypeDef SessionFSM_SetBankFrequency(DacBank_t bank, uint32_t freq_hz)
{
    g_fsm.mutations++;
    g_fsm.setfreq_calls++;
    g_fsm.setfreq_bank = (int)bank;
    g_fsm.setfreq_hz = freq_hz;
    return g_fsm.ret;
}

HAL_StatusTypeDef SessionFSM_SetWell(uint8_t channel, bool connected)
{
    g_fsm.mutations++;
    g_fsm.setwell_calls++;
    g_fsm.setwell_channel = channel;
    g_fsm.setwell_connected = connected;
    return g_fsm.ret;
}

HAL_StatusTypeDef SessionFSM_Start(uint32_t on_seconds, uint32_t off_seconds, uint32_t duration_seconds)
{
    g_fsm.mutations++;
    g_fsm.start_calls++;
    g_fsm.start_on_s = on_seconds;
    g_fsm.start_off_s = off_seconds;
    g_fsm.start_duration_s = duration_seconds;
    return g_fsm.ret;
}

HAL_StatusTypeDef SessionFSM_Stop(void)
{
    g_fsm.mutations++;
    g_fsm.stop_calls++;
    return g_fsm.ret;
}

/* GetStatus NO cuenta como mutacion: STATUS puede consultarlo aunque el
 * resto de asserts exijan "cero llamadas a la FSM". Devuelve una foto
 * deterministica para que el formateo de la linea STATUS tenga datos. */
void SessionFSM_GetStatus(SessionStatus_t *out)
{
    g_fsm.getstatus_calls++;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = SESSION_STATE_IDLE;
}
