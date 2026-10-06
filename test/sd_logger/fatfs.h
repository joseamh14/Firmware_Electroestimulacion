/**
 * @file  fatfs.h (arnes)
 * @brief FatFs minimo simulado en RAM para el arnes de sd_logger.
 */
#ifndef TEST_FATFS_H_
#define TEST_FATFS_H_
#include <stdint.h>

typedef char     TCHAR;
typedef unsigned UINT;
typedef uint8_t  BYTE;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NOT_READY = 3, FR_NO_FILE = 4, FR_DENIED = 7,
               FR_EXIST = 8 } FRESULT;
typedef struct { int dummy; } FATFS;
typedef struct { int idx; } FIL;
typedef struct { uint32_t fsize; } FILINFO;

#define FA_WRITE       0x02
#define FA_CREATE_NEW  0x04

extern FATFS SDFatFS;
extern char  SDPath[4];

FRESULT f_mount(FATFS *fs, const TCHAR *path, BYTE opt);
FRESULT f_stat(const TCHAR *path, FILINFO *fno);
FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_sync(FIL *fp);
FRESULT f_close(FIL *fp);

/* --- controles del simulador --- */
#define SIMFS_MAX_FILES  8
#define SIMFS_MAX_BYTES  4096
typedef struct {
    int      card_present;
    int      fail_write_after;      /* -1 = nunca; n = falla la escritura n+1 */
    int      mounts, syncs, opens, closes, writes;
    int      nfiles;
    char     name[SIMFS_MAX_FILES][13];
    uint8_t  data[SIMFS_MAX_FILES][SIMFS_MAX_BYTES];
    uint32_t size[SIMFS_MAX_FILES];
    uint32_t synced[SIMFS_MAX_FILES];
    int      open_idx;              /* -1 = ninguno */
} SimFs_t;
extern SimFs_t g_fs;
void SimFs_Reset(void);
#endif
