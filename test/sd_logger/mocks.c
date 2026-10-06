/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   FatFs en RAM, estado de la sesion, lector de corriente y reloj
 *          simulados para el arnes de sd_logger.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "fatfs.h"
#include "session_fsm.h"
#include "tia_reader.h"
#include "flash_log.h"
#include "test_spy.h"
#include <string.h>

FATFS SDFatFS;
char  SDPath[4] = "0:/";
SimFs_t g_fs;
LogSpy_t g_spy;

void SimFs_Reset(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    g_fs.card_present = 1;
    g_fs.fail_write_after = -1;
    g_fs.open_idx = -1;
}

static int find(const TCHAR *path)
{
    for (int i = 0; i < g_fs.nfiles; i++) {
        if (strcmp(g_fs.name[i], path) == 0) return i;
    }
    return -1;
}

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt)
{
    (void)fs; (void)path; (void)opt;
    g_fs.mounts++;
    return g_fs.card_present ? FR_OK : FR_NOT_READY;
}

FRESULT f_stat(const TCHAR *path, FILINFO *fno)
{
    if (!g_fs.card_present) return FR_NOT_READY;
    int i = find(path);
    if (i < 0) return FR_NO_FILE;
    fno->fsize = g_fs.size[i];
    return FR_OK;
}

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    (void)mode;
    if (!g_fs.card_present) return FR_NOT_READY;
    if (find(path) >= 0) return FR_EXIST;
    if (g_fs.nfiles >= SIMFS_MAX_FILES) return FR_DENIED;
    int i = g_fs.nfiles++;
    strncpy(g_fs.name[i], path, 12);
    g_fs.size[i] = 0;
    fp->idx = i;
    g_fs.open_idx = i;
    g_fs.opens++;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    g_fs.writes++;
    g_spy.write_while_paused += g_spy.paused ? 1 : 0;
    if (g_fs.fail_write_after == 0) { *bw = 0; return FR_DISK_ERR; }
    if (g_fs.fail_write_after > 0) g_fs.fail_write_after--;
    int i = fp->idx;
    if (g_fs.size[i] + btw > SIMFS_MAX_BYTES) { *bw = 0; return FR_OK; }   /* llena */
    memcpy(&g_fs.data[i][g_fs.size[i]], buff, btw);
    g_fs.size[i] += btw;
    *bw = btw;
    return FR_OK;
}

FRESULT f_sync(FIL *fp)
{
    g_fs.syncs++;
    g_fs.synced[fp->idx] = g_fs.size[fp->idx];
    return FR_OK;
}

FRESULT f_close(FIL *fp)
{
    g_fs.closes++;
    g_fs.synced[fp->idx] = g_fs.size[fp->idx];
    g_fs.open_idx = -1;
    return FR_OK;
}

/* --- reloj, sesion, lector --- */
uint32_t HAL_GetTick(void) { return g_spy.tick; }
SessionState_t SessionFSM_GetState(void) { return g_spy.state; }

void TiaReader_Start(uint8_t mask) { g_spy.starts++; g_spy.mask = mask; g_spy.running = true; }
void TiaReader_Stop(void) { g_spy.stops++; g_spy.running = false; }
void TiaReader_Pause(void) { g_spy.pauses++; g_spy.paused = true; }
void TiaReader_Resume(void) { g_spy.paused = false; }
void TiaReader_TakeWindow(TiaWindow_t *out)
{
    g_spy.takes++;
    memcpy(out, &g_spy.win, sizeof(*out));
}

HAL_StatusTypeDef FlashLog_ReadLatest(const FlashLog_Config_t *cfg, void *payload,
                                      uint32_t payload_len, uint32_t *out_seq)
{ (void)cfg; (void)payload; (void)payload_len; (void)out_seq; return HAL_ERROR; }
HAL_StatusTypeDef FlashLog_Append(const FlashLog_Config_t *cfg, const void *payload,
                                  uint32_t payload_len)
{ (void)cfg; (void)payload; (void)payload_len; return HAL_OK; }
