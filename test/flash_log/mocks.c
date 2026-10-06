/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   Simulador de la flash (ultimos 32 KB) para el arnes de flash_log.
 *
 * Reproduce las reglas que importan para la robustez del registro:
 *  - solo se programa una doble palabra borrada (si no, error, como el
 *    PROGERR del STM32L4);
 *  - el borrado es por sector logico de 8 KB;
 *  - un corte de alimentacion programado (SimFlash_PowerCutAfter) deja la
 *    operacion en curso a medias: la doble palabra queda con un valor
 *    arbitrario y error ECC doble (o, con SimFlash_CutClean(true), el corte
 *    cae justo entre dos programaciones y la doble palabra sigue borrada:
 *    solo el CRC puede detectar el registro incompleto), o el sector a
 *    medio borrar con errores ECC en la parte no borrada. Desde ese momento ninguna operacion tiene
 *    efecto hasta SimFlash_Reboot();
 *  - leer una doble palabra con error ECC doble activa FlashLog_EccFault,
 *    como hace el manejador de NMI del firmware.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "flash_log.h"
#include <string.h>

#define SIM_BASE   (0x081F8000UL)
#define SIM_SIZE   (0x8000UL)
#define SIM_DWS    (SIM_SIZE / 8U)

static uint8_t  s_mem[SIM_SIZE];
static bool     s_ecc_bad[SIM_DWS];
static int      s_ops_left;       /* -1 = sin corte programado */
static bool     s_down;
static uint32_t s_image_end;
static int      s_erases[4];
static int      s_programs;
static bool     s_cut_clean;      /* corte entre operaciones, sin ECC */

static uint32_t Off(uint32_t addr) { return addr - SIM_BASE; }

void SimFlash_Reset(void)
{
    memset(s_mem, 0xFF, sizeof(s_mem));
    memset(s_ecc_bad, 0, sizeof(s_ecc_bad));
    memset(s_erases, 0, sizeof(s_erases));
    s_ops_left = -1;
    s_down = false;
    s_image_end = 0x08020000UL;
    s_programs = 0;
    s_cut_clean = false;
}

void SimFlash_CutClean(bool clean) { s_cut_clean = clean; }

void SimFlash_PowerCutAfter(int ops) { s_ops_left = ops; }
void SimFlash_Reboot(void)           { s_down = false; s_ops_left = -1; FlashLog_EccFault = false; }
bool SimFlash_IsDown(void)           { return s_down; }
void SimFlash_SetImageEnd(uint32_t a){ s_image_end = a; }
int  SimFlash_ProgramCount(void)     { return s_programs; }
uint8_t *SimFlash_Raw(uint32_t addr) { return &s_mem[Off(addr)]; }

int SimFlash_EraseCount(uint32_t sector_addr)
{
    return s_erases[Off(sector_addr) / FLASH_LOG_SECTOR_SIZE];
}

void SimFlash_CorruptDw(uint32_t addr)
{
    uint32_t o = Off(addr) & ~7UL;
    memset(&s_mem[o], 0x5A, 8);
    s_ecc_bad[o / 8U] = true;
}

/* Devuelve true si esta operacion es la que sufre el corte. */
static bool CutNow(void)
{
    if (s_ops_left < 0) {
        return false;
    }
    if (s_ops_left == 0) {
        s_down = true;
        s_ops_left = -1;
        return true;
    }
    s_ops_left--;
    return false;
}

uint64_t FlashPort_Read64(uint32_t addr)
{
    uint32_t o = Off(addr);
    uint64_t v;
    memcpy(&v, &s_mem[o], 8);
    if (s_ecc_bad[o / 8U]) {
        FlashLog_EccFault = true;   /* lo que haria la NMI */
    }
    return v;
}

HAL_StatusTypeDef FlashPort_Program64(uint32_t addr, uint64_t value)
{
    if (s_down) {
        return HAL_ERROR;
    }
    uint32_t o = Off(addr);
    if ((o & 7U) != 0U) {
        return HAL_ERROR;
    }
    if (CutNow()) {
        if (!s_cut_clean) {
            SimFlash_CorruptDw(addr);   /* programacion interrumpida */
        }                               /* limpio: la doble palabra sigue borrada */
        return HAL_ERROR;
    }
    uint64_t cur;
    memcpy(&cur, &s_mem[o], 8);
    if (cur != 0xFFFFFFFFFFFFFFFFULL || s_ecc_bad[o / 8U]) {
        return HAL_ERROR;           /* PROGERR: la doble palabra no estaba borrada */
    }
    memcpy(&s_mem[o], &value, 8);
    s_programs++;
    return HAL_OK;
}

HAL_StatusTypeDef FlashPort_EraseSector(uint32_t addr)
{
    if (s_down) {
        return HAL_ERROR;
    }
    uint32_t o = Off(addr);
    if ((o % FLASH_LOG_SECTOR_SIZE) != 0U) {
        return HAL_ERROR;
    }
    if (CutNow()) {
        /* borrado a medias: primera mitad borrada, el resto con ECC doble */
        memset(&s_mem[o], 0xFF, FLASH_LOG_SECTOR_SIZE / 2U);
        memset(&s_ecc_bad[o / 8U], 0, (FLASH_LOG_SECTOR_SIZE / 2U) / 8U);
        for (uint32_t k = FLASH_LOG_SECTOR_SIZE / 2U; k < FLASH_LOG_SECTOR_SIZE; k += 8U) {
            SimFlash_CorruptDw(addr + k);
        }
        return HAL_ERROR;
    }
    memset(&s_mem[o], 0xFF, FLASH_LOG_SECTOR_SIZE);
    memset(&s_ecc_bad[o / 8U], 0, FLASH_LOG_SECTOR_SIZE / 8U);
    s_erases[o / FLASH_LOG_SECTOR_SIZE]++;
    return HAL_OK;
}

uint32_t FlashPort_ImageEnd(void)
{
    return s_image_end;
}
