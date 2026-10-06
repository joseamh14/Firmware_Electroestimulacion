/**
 ******************************************************************************
 * @file    mocks.c
 * @brief   Implementaciones de escritorio de la HAL del RTC y de la capa BSP
 *          para el arnes de pruebas de session_fsm.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "ad5144.h"
#include "dac_waveform.h"
#include "culture_selector.h"
#include "flash_log.h"
#include <string.h>

/* ===================================================================== *
 *  Reloj falso del RTC
 * ===================================================================== */
static uint64_t g_unix;   /* segundos desde 1970-01-01 00:00:00 */

void MockRTC_SetUnix(uint64_t seconds)       { g_unix = seconds; }
void MockRTC_AdvanceSeconds(uint32_t seconds){ g_unix += (uint64_t)seconds; }
uint64_t MockRTC_GetUnix(void)               { return g_unix; }

/* Algoritmo de fecha civil <- dias (H. Hinnant, dominio publico). Es el
 * inverso exacto del que usa session_fsm.c (alli sin el -719468). */
static void CivilFromDays(long long z, int *py, unsigned *pm, unsigned *pd)
{
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned  doe = (unsigned)(z - era * 146097);                 /* [0,146096] */
    unsigned  yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365; /* [0,399] */
    long long y   = (long long)yoe + era * 400;
    unsigned  doy = doe - (365*yoe + yoe/4 - yoe/100);            /* [0,365] */
    unsigned  mp  = (5*doy + 2) / 153;                            /* [0,11]  */
    unsigned  d   = doy - (153*mp + 2)/5 + 1;                     /* [1,31]  */
    unsigned  m   = mp < 10 ? mp + 3 : mp - 9;                    /* [1,12]  */
    *py = (int)(y + (m <= 2));
    *pm = m;
    *pd = d;
}

void HAL_PWR_EnableBkUpAccess(void) { /* sin efecto en escritorio */ }

HAL_StatusTypeDef HAL_RTC_GetTime(RTC_HandleTypeDef *hrtc, RTC_TimeTypeDef *sTime, uint32_t Format)
{
    (void)hrtc; (void)Format;
    uint32_t sod = (uint32_t)(g_unix % 86400ULL);
    sTime->Hours   = (uint8_t)(sod / 3600U);
    sTime->Minutes = (uint8_t)((sod % 3600U) / 60U);
    sTime->Seconds = (uint8_t)(sod % 60U);
    sTime->SubSeconds = 0U;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_RTC_GetDate(RTC_HandleTypeDef *hrtc, RTC_DateTypeDef *sDate, uint32_t Format)
{
    (void)hrtc; (void)Format;
    long long days = (long long)(g_unix / 86400ULL);
    int y; unsigned m, d;
    CivilFromDays(days, &y, &m, &d);
    sDate->Year  = (uint8_t)(y - 2000);   /* session hace 2000 + Year */
    sDate->Month = (uint8_t)m;
    sDate->Date  = (uint8_t)d;
    sDate->WeekDay = 1U;
    return HAL_OK;
}

/* ===================================================================== *
 *  Registros de backup (persisten entre reinicios simulados)
 * ===================================================================== */
static uint32_t g_bkp[MOCK_BKP_COUNT];

void      MockBKP_Reset(void)      { for (unsigned i = 0; i < MOCK_BKP_COUNT; i++) g_bkp[i] = 0U; }
uint32_t *MockBKP_Raw(void)        { return g_bkp; }

uint32_t HAL_RTCEx_BKUPRead(RTC_HandleTypeDef *hrtc, uint32_t reg)
{
    (void)hrtc;
    return (reg < MOCK_BKP_COUNT) ? g_bkp[reg] : 0U;
}

void HAL_RTCEx_BKUPWrite(RTC_HandleTypeDef *hrtc, uint32_t reg, uint32_t data)
{
    (void)hrtc;
    if (reg < MOCK_BKP_COUNT) g_bkp[reg] = data;
}

/* ===================================================================== *
 *  Espias
 * ===================================================================== */
MockSpy_t g_spy;
void MockSpy_Reset(void) { memset(&g_spy, 0, sizeof(g_spy)); }

/* ===================================================================== *
 *  Capa BSP simulada
 * ===================================================================== */
HAL_StatusTypeDef AD5144_Init(AD5144_Handle_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
    if (dev == NULL) return HAL_ERROR;
    dev->hi2c  = hi2c;
    dev->addr7 = addr7;
    dev->shadow[0] = dev->shadow[1] = dev->shadow[2] = dev->shadow[3] = 0U;
    return HAL_OK;
}

HAL_StatusTypeDef AD5144_SetChannelRaw(AD5144_Handle_t *dev, AD5144_Channel_t ch, uint8_t code)
{
    if (dev == NULL) return HAL_ERROR;
    dev->shadow[(int)ch] = code;
    return HAL_OK;
}

HAL_StatusTypeDef AD5144_SetChannelMillivolts(AD5144_Handle_t *dev, AD5144_Channel_t ch,
                                              uint16_t target_mv, uint16_t full_scale_mv)
{
    (void)full_scale_mv;
    if (dev == NULL) return HAL_ERROR;
    int base = (dev->addr7 == AD5144_I2C_ADDR_ADDR_VLOG) ? 4 : 0;
    g_spy.ad_setmv_calls++;
    if ((g_spy.ad_fail_mask >> (base + (int)ch)) & 1U) return HAL_ERROR;
    g_spy.ad_setmv_last_mv[base + (int)ch] = target_mv;
    return HAL_OK;
}

HAL_StatusTypeDef AD5144_AllChannelsZero(AD5144_Handle_t *dev)
{
    if (dev == NULL) return HAL_ERROR;
    int base = (dev->addr7 == AD5144_I2C_ADDR_ADDR_VLOG) ? 4 : 0;
    g_spy.ad_allzero_calls++;
    for (int i = 0; i < 4; i++) g_spy.ad_setmv_last_mv[base + i] = 0U;
    return HAL_OK;
}

HAL_StatusTypeDef DacWaveform_Init(DAC_HandleTypeDef *hdac, TIM_HandleTypeDef *a, TIM_HandleTypeDef *b)
{
    (void)hdac; (void)a; (void)b;
    return HAL_OK;
}

HAL_StatusTypeDef DacWaveform_SetFrequency(DacBank_t bank, uint32_t freq_hz)
{
    if (bank != DAC_BANK_A && bank != DAC_BANK_B) return HAL_ERROR;
    g_spy.dac_setfreq_calls++;
    if (g_spy.dac_fail && freq_hz != 0U) return HAL_ERROR;
    g_spy.dac_setfreq_last[(int)bank] = freq_hz;
    return HAL_OK;
}

HAL_StatusTypeDef DacWaveform_Stop(DacBank_t bank)
{
    if (bank != DAC_BANK_A && bank != DAC_BANK_B) return HAL_ERROR;
    g_spy.dac_stop_calls[(int)bank]++;
    return HAL_OK;
}

uint32_t DacWaveform_GetFrequency(DacBank_t bank)
{
    return (bank == DAC_BANK_A || bank == DAC_BANK_B) ? g_spy.dac_setfreq_last[(int)bank] : 0U;
}

HAL_StatusTypeDef CultureSelector_SetWell(uint8_t well, bool connected)
{
    if (well < CULTURE_WELL_MIN || well > CULTURE_WELL_MAX) return HAL_ERROR;
    g_spy.sel_setwell_calls++;
    if (connected) g_spy.sel_well_state |=  (uint8_t)(1U << (well - 1U));
    else           g_spy.sel_well_state &= (uint8_t)~(1U << (well - 1U));
    return HAL_OK;
}

HAL_StatusTypeDef CultureSelector_AllDisconnect(void)
{
    g_spy.sel_alldisconnect_calls++;
    g_spy.sel_well_state = 0U;
    return HAL_OK;
}

HAL_StatusTypeDef AD5144_StoreSafePowerUp(AD5144_Handle_t *dev)
{
    if (dev == NULL) return HAL_ERROR;
    g_spy.ad_eeprom_calls++;
    return HAL_OK;
}

/* ===================================================================== *
 *  flash_log simulado: un unico registro (el ultimo), persistente
 * ===================================================================== */
static uint32_t g_flash_words[FLASH_LOG_MAX_RECORD / 4U];
static uint32_t g_flash_len;
static bool     g_flash_valid;
static int      g_flash_appends;
static bool     g_flash_fail;

void MockFlash_Reset(void)
{
    memset(g_flash_words, 0, sizeof(g_flash_words));
    g_flash_len = 0U;
    g_flash_valid = false;
    g_flash_appends = 0;
    g_flash_fail = false;
}
int             MockFlash_AppendCount(void)       { return g_flash_appends; }
const uint32_t *MockFlash_LastWords(void)         { return g_flash_words; }
uint32_t       *MockFlash_LastWordsRaw(void)      { return g_flash_words; }
void            MockFlash_SetWriteFail(bool fail) { g_flash_fail = fail; }

HAL_StatusTypeDef FlashLog_Append(const FlashLog_Config_t *cfg, const void *payload,
                                  uint32_t payload_len)
{
    if (cfg == NULL || payload == NULL || payload_len > sizeof(g_flash_words)) return HAL_ERROR;
    if (g_flash_fail) return HAL_ERROR;
    memcpy(g_flash_words, payload, payload_len);
    g_flash_len = payload_len;
    g_flash_valid = true;
    g_flash_appends++;
    return HAL_OK;
}

HAL_StatusTypeDef FlashLog_ReadLatest(const FlashLog_Config_t *cfg, void *payload,
                                      uint32_t payload_len, uint32_t *out_seq)
{
    if (cfg == NULL || payload == NULL || !g_flash_valid || payload_len > g_flash_len) return HAL_ERROR;
    memcpy(payload, g_flash_words, payload_len);
    if (out_seq != NULL) *out_seq = (uint32_t)g_flash_appends;
    return HAL_OK;
}
