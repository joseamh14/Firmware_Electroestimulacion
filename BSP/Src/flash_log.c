/**
 ******************************************************************************
 * @file    flash_log.c
 * @brief   Registro de solo anexado en la flash interna, tolerante a cortes.
 ******************************************************************************
 */
#include "flash_log.h"
#include <string.h>

#define FLASH_LOG_ERASED_DW   (0xFFFFFFFFFFFFFFFFULL)

volatile bool FlashLog_EccFault = false;

/* ------------------------------------------------------------------------- *
 *  Capa de acceso a la flash. En el arnes de escritorio
 *  (FLASH_LOG_HOST_TEST) estas cuatro funciones las aporta el propio arnes
 *  sobre un array en RAM con inyeccion de fallos.
 * ------------------------------------------------------------------------- */
#ifdef FLASH_LOG_HOST_TEST
uint64_t          FlashPort_Read64(uint32_t addr);
HAL_StatusTypeDef FlashPort_Program64(uint32_t addr, uint64_t value);
HAL_StatusTypeDef FlashPort_EraseSector(uint32_t addr);
uint32_t          FlashPort_ImageEnd(void);
#else

/* STM32L4R5ZI: 2 MB. Con DBANK = 1, el banco 2 empieza en la mitad. Se
 * supone BFB2 = 0 (sin intercambio de bancos), el valor de fabrica. */
#define FLASH_LOG_DEVICE_SIZE   (0x200000UL)
#define FLASH_LOG_BANK2_BASE    (FLASH_BASE + (FLASH_LOG_DEVICE_SIZE / 2U))

static uint64_t FlashPort_Read64(uint32_t addr)
{
    return *(volatile const uint64_t *)addr;
}

static void FlushDataCache(void)
{
    /* Tras programar, la cache de datos del ART podria devolver el valor
     * borrado antiguo de una linea ya cacheada. */
    if (READ_BIT(FLASH->ACR, FLASH_ACR_DCEN) != 0U) {
        __HAL_FLASH_DATA_CACHE_DISABLE();
        __HAL_FLASH_DATA_CACHE_RESET();
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }
}

static HAL_StatusTypeDef FlashPort_Program64(uint32_t addr, uint64_t value)
{
    HAL_StatusTypeDef st = HAL_FLASH_Unlock();
    if (st != HAL_OK) {
        return st;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, value);
    (void)HAL_FLASH_Lock();
    FlushDataCache();
    return st;
}

static HAL_StatusTypeDef FlashPort_EraseSector(uint32_t addr)
{
    FLASH_EraseInitTypeDef er = {0};
    uint32_t page_error = 0U;

    er.TypeErase = FLASH_TYPEERASE_PAGES;
    if (READ_BIT(FLASH->OPTR, FLASH_OPTR_DBANK) != 0U) {
        /* Doble banco: paginas de 4 KB -> 2 paginas por sector logico */
        if (addr >= FLASH_LOG_BANK2_BASE) {
            er.Banks = FLASH_BANK_2;
            er.Page  = (addr - FLASH_LOG_BANK2_BASE) / FLASH_PAGE_SIZE;
        } else {
            er.Banks = FLASH_BANK_1;
            er.Page  = (addr - FLASH_BASE) / FLASH_PAGE_SIZE;
        }
        er.NbPages = FLASH_LOG_SECTOR_SIZE / FLASH_PAGE_SIZE;
    } else {
        /* Banco unico: paginas de 8 KB -> 1 pagina por sector logico */
        er.Banks   = FLASH_BANK_1;
        er.Page    = (addr - FLASH_BASE) / FLASH_PAGE_SIZE_128_BITS;
        er.NbPages = FLASH_LOG_SECTOR_SIZE / FLASH_PAGE_SIZE_128_BITS;
    }

    HAL_StatusTypeDef st = HAL_FLASH_Unlock();
    if (st != HAL_OK) {
        return st;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    st = HAL_FLASHEx_Erase(&er, &page_error);   /* vacia tambien las caches */
    (void)HAL_FLASH_Lock();
    return st;
}

/* Fin de la imagen en flash: fin de .text/.rodata mas la copia de .data
 * (simbolos del script de enlazado generado por CubeIDE). */
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;

static uint32_t FlashPort_ImageEnd(void)
{
    return (uint32_t)&_sidata + ((uint32_t)&_edata - (uint32_t)&_sdata);
}
#endif /* FLASH_LOG_HOST_TEST */

/* ------------------------------------------------------------------------- *
 *  Utilidades
 * ------------------------------------------------------------------------- */
uint32_t FlashLog_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint8_t k = 0U; k < 8U; k++) {
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

static bool ConfigValid(const FlashLog_Config_t *cfg, uint32_t payload_len)
{
    if (cfg == NULL) {
        return false;
    }
    if (cfg->record_size == 0U || cfg->record_size > FLASH_LOG_MAX_RECORD ||
        (cfg->record_size % FLASH_LOG_ALIGN) != 0U) {
        return false;
    }
    if (FLASH_LOG_HEADER_BYTES + payload_len + FLASH_LOG_CRC_BYTES > cfg->record_size) {
        return false;
    }
    if (cfg->magic == 0xFFFFFFFFUL) {
        return false;   /* indistinguible de flash borrada */
    }
    if (!FlashLog_RegionIsFree()) {
        return false;   /* la imagen ha crecido hasta la zona reservada */
    }
    return true;
}

static uint32_t SlotsPerSector(const FlashLog_Config_t *cfg)
{
    return FLASH_LOG_SECTOR_SIZE / cfg->record_size;
}

static uint32_t SlotAddr(const FlashLog_Config_t *cfg, uint8_t sector, uint32_t slot)
{
    return cfg->sector[sector] + slot * cfg->record_size;
}

/* Copia un registro de flash a RAM. Devuelve false si la lectura ha
 * provocado un error ECC doble (registro corrupto). */
static bool ReadRecord(const FlashLog_Config_t *cfg, uint32_t addr, uint8_t *buf)
{
    FlashLog_EccFault = false;
    for (uint32_t off = 0U; off < cfg->record_size; off += 8U) {
        uint64_t dw = FlashPort_Read64(addr + off);
        memcpy(&buf[off], &dw, sizeof(dw));
    }
    bool ok = !FlashLog_EccFault;
    FlashLog_EccFault = false;
    return ok;
}

static bool SlotIsErased(const FlashLog_Config_t *cfg, uint32_t addr)
{
    FlashLog_EccFault = false;
    bool erased = true;
    for (uint32_t off = 0U; off < cfg->record_size; off += 8U) {
        if (FlashPort_Read64(addr + off) != FLASH_LOG_ERASED_DW) {
            erased = false;
            break;
        }
    }
    if (FlashLog_EccFault) {
        erased = false;   /* doble palabra corrupta: el hueco esta usado */
    }
    FlashLog_EccFault = false;
    return erased;
}

/* Valida un registro ya copiado a RAM: marca, CRC. */
static bool RecordValid(const FlashLog_Config_t *cfg, const uint8_t *buf, uint32_t *seq)
{
    uint32_t magic;
    uint32_t crc_stored;
    memcpy(&magic, &buf[0], 4U);
    if (magic != cfg->magic) {
        return false;
    }
    memcpy(&crc_stored, &buf[cfg->record_size - FLASH_LOG_CRC_BYTES], 4U);
    if (FlashLog_Crc32(buf, cfg->record_size - FLASH_LOG_CRC_BYTES) != crc_stored) {
        return false;
    }
    memcpy(seq, &buf[4], 4U);
    return true;
}

typedef struct {
    bool     found;
    uint32_t seq;
    uint8_t  sector;
    uint32_t slot;
} LatestInfo_t;

static void FindLatest(const FlashLog_Config_t *cfg, LatestInfo_t *info, uint8_t *buf_latest)
{
    uint8_t buf[FLASH_LOG_MAX_RECORD];
    info->found = false;
    info->seq = 0U;
    info->sector = 0U;
    info->slot = 0U;

    for (uint8_t s = 0U; s < 2U; s++) {
        for (uint32_t i = 0U; i < SlotsPerSector(cfg); i++) {
            uint32_t addr = SlotAddr(cfg, s, i);
            if (SlotIsErased(cfg, addr)) {
                break;   /* los huecos se usan en orden: no hay nada despues */
            }
            uint32_t seq;
            if (!ReadRecord(cfg, addr, buf) || !RecordValid(cfg, buf, &seq)) {
                continue;
            }
            /* Comparacion con signo de la diferencia: robusta al desbordamiento */
            if (!info->found || (int32_t)(seq - info->seq) > 0) {
                info->found = true;
                info->seq = seq;
                info->sector = s;
                info->slot = i;
                if (buf_latest != NULL) {
                    memcpy(buf_latest, buf, cfg->record_size);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------- *
 *  API publica
 * ------------------------------------------------------------------------- */
HAL_StatusTypeDef FlashLog_ReadLatest(const FlashLog_Config_t *cfg, void *payload,
                                      uint32_t payload_len, uint32_t *out_seq)
{
    if (!ConfigValid(cfg, payload_len) || payload == NULL) {
        return HAL_ERROR;
    }

    uint8_t buf[FLASH_LOG_MAX_RECORD];
    LatestInfo_t info;
    FindLatest(cfg, &info, buf);
    if (!info.found) {
        return HAL_ERROR;
    }

    memcpy(payload, &buf[FLASH_LOG_HEADER_BYTES], payload_len);
    if (out_seq != NULL) {
        *out_seq = info.seq;
    }
    return HAL_OK;
}

HAL_StatusTypeDef FlashLog_Append(const FlashLog_Config_t *cfg, const void *payload,
                                  uint32_t payload_len)
{
    if (!ConfigValid(cfg, payload_len) || payload == NULL) {
        return HAL_ERROR;
    }

    LatestInfo_t info;
    FindLatest(cfg, &info, NULL);

    /* Hueco de destino: primer hueco borrado del sector activo a partir del
     * ultimo registro valido. Si no queda, se borra el otro sector. */
    uint8_t  sector = info.found ? info.sector : 0U;
    uint32_t seq    = info.found ? (info.seq + 1U) : 1U;
    uint32_t start  = info.found ? (info.slot + 1U) : 0U;
    bool     have_slot = false;
    uint32_t slot = 0U;

    for (uint32_t i = start; i < SlotsPerSector(cfg); i++) {
        if (SlotIsErased(cfg, SlotAddr(cfg, sector, i))) {
            slot = i;
            have_slot = true;
            break;
        }
    }

    if (!have_slot) {
        /* Sin registro previo y sector 0 sucio, o sector activo lleno: se
         * pasa al otro sector, que se borra entero. El sector activo no se
         * toca hasta que el nuevo registro este escrito. */
        sector = info.found ? (uint8_t)(1U - info.sector) : 0U;
        HAL_StatusTypeDef st = FlashPort_EraseSector(cfg->sector[sector]);
        if (st != HAL_OK) {
            return st;
        }
        slot = 0U;
    }

    uint8_t buf[FLASH_LOG_MAX_RECORD];
    memset(buf, 0, sizeof(buf));
    memcpy(&buf[0], &cfg->magic, 4U);
    memcpy(&buf[4], &seq, 4U);
    memcpy(&buf[FLASH_LOG_HEADER_BYTES], payload, payload_len);
    uint32_t crc = FlashLog_Crc32(buf, cfg->record_size - FLASH_LOG_CRC_BYTES);
    memcpy(&buf[cfg->record_size - FLASH_LOG_CRC_BYTES], &crc, 4U);

    uint32_t addr = SlotAddr(cfg, sector, slot);
    for (uint32_t off = 0U; off < cfg->record_size; off += 8U) {
        uint64_t dw;
        memcpy(&dw, &buf[off], sizeof(dw));
        HAL_StatusTypeDef st = FlashPort_Program64(addr + off, dw);
        if (st != HAL_OK) {
            return st;
        }
    }
    return HAL_OK;
}

bool FlashLog_RegionIsFree(void)
{
    return FlashPort_ImageEnd() <= FLASH_LOG_REGION_START;
}
