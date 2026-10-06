/**
 ******************************************************************************
 * @file    sd_logger.c
 * @brief   Medida de corriente por tramos y registro binario en la microSD.
 ******************************************************************************
 */
#include "sd_logger.h"
#include "session_fsm.h"
#include "tia_reader.h"
#include "fatfs.h"
#include <stdio.h>
#include <string.h>

#define SD_LOGGER_ALL_CHANNELS  (0xFFU)

static SdLogState_t s_state;
static bool     s_mounted;
static bool     s_file_open;
static FIL      s_fil;
static char     s_name[13];
static uint32_t s_t0;
static uint32_t s_next;
static uint32_t s_records;
static bool     s_have_values;
static float    s_last[CURRENT_LOG_NUM_CH];

static bool MountCard(void)
{
    if (!s_mounted) {
        s_mounted = (f_mount(&SDFatFS, (TCHAR const *)SDPath, 1) == FR_OK);
    }
    return s_mounted;
}

static bool OpenNewFile(void)
{
    FILINFO fno;
    for (uint32_t n = 1U; n <= SD_LOGGER_MAX_FILES; n++) {
        (void)snprintf(s_name, sizeof(s_name), "B%04lu.BIN", (unsigned long)n);
        if (f_stat(s_name, &fno) == FR_NO_FILE) {
            if (f_open(&s_fil, s_name, FA_CREATE_NEW | FA_WRITE) == FR_OK) {
                return true;
            }
            break;
        }
    }
    s_name[0] = '\0';
    return false;
}

static void CloseFile(void)
{
    if (s_file_open) {
        (void)f_close(&s_fil);
        s_file_open = false;
    }
}

static void BeginBlock(void)
{
    TiaReader_Start(SD_LOGGER_ALL_CHANNELS);
    s_t0 = HAL_GetTick();
    s_next = s_t0 + SD_LOGGER_PERIOD_MS;
    s_records = 0U;
    s_name[0] = '\0';

    s_file_open = MountCard() && OpenNewFile();
    if (!s_file_open) {
        s_mounted = false;          /* se reintenta el montaje en el siguiente tramo */
        s_state = SD_LOG_NO_CARD;
    } else {
        s_state = SD_LOG_ACTIVE;
    }
}

static void EndBlock(void)
{
    TiaReader_Stop();
    CloseFile();
    s_state = SD_LOG_OFF;
}

static void CloseWindow(uint32_t now)
{
    TiaWindow_t w;
    TiaReader_TakeWindow(&w);
    CurrentLog_Compute(&w, s_last);
    s_have_values = true;

    if (!s_file_open) {
        return;
    }

    uint8_t rec[CURRENT_LOG_RECORD_BYTES];
    CurrentLog_Pack(now - s_t0, s_last, rec);

    TiaReader_Pause();
    UINT written = 0U;
    FRESULT fr = f_write(&s_fil, rec, sizeof(rec), &written);
    if (fr == FR_OK && written == sizeof(rec)) {
        s_records++;
        if ((s_records % SD_LOGGER_SYNC_EVERY) == 0U) {
            fr = f_sync(&s_fil);
        }
    } else if (fr == FR_OK) {
        fr = FR_DENIED;   /* tarjeta llena */
    }
    TiaReader_Resume();

    if (fr != FR_OK) {
        CloseFile();
        s_mounted = false;
        s_state = SD_LOG_NO_CARD;   /* se sigue midiendo sin registrar */
    }
}

void SdLogger_Init(void)
{
    s_state = SD_LOG_OFF;
    s_mounted = false;
    s_file_open = false;
    s_name[0] = '\0';
    s_records = 0U;
    s_have_values = false;
    for (uint8_t i = 0U; i < CURRENT_LOG_NUM_CH; i++) {
        s_last[i] = CurrentLog_NaN();
    }
}

void SdLogger_Task(void)
{
    bool stim_on = (SessionFSM_GetState() == SESSION_STATE_STIM_ON);

    if (stim_on && s_state == SD_LOG_OFF) {
        BeginBlock();
        return;
    }
    if (!stim_on && s_state != SD_LOG_OFF) {
        EndBlock();
        return;
    }
    if (s_state == SD_LOG_OFF) {
        return;
    }

    uint32_t now = HAL_GetTick();
    if ((int32_t)(now - s_next) >= 0) {
        CloseWindow(now);
        s_next += SD_LOGGER_PERIOD_MS;
        if ((int32_t)(now - s_next) >= 0) {
            s_next = now + SD_LOGGER_PERIOD_MS;   /* bucle muy retrasado: no acumular */
        }
    }
}

void SdLogger_GetStatus(SdLogStatus_t *out)
{
    if (out == NULL) {
        return;
    }
    out->state = s_state;
    memcpy(out->file, s_name, sizeof(out->file));
    out->records = s_records;
    out->have_values = s_have_values;
    memcpy(out->last_ua, s_last, sizeof(out->last_ua));
}
