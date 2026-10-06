/**
 ******************************************************************************
 * @file    tia_reader.c
 * @brief   Medida de la corriente de estimulacion por rafagas del ADC1.
 ******************************************************************************
 */
#include "tia_reader.h"
#include "tia_mux.h"
#include "dac_waveform.h"
#include <string.h>

/* ------------------------------------------------------------------------- *
 *  Capa dependiente del nucleo. En el arnes de escritorio
 *  (TIA_READER_HOST_TEST) la aporta el propio arnes.
 * ------------------------------------------------------------------------- */
#ifdef TIA_READER_HOST_TEST
void     TiaPort_InitTiming(void);
void     TiaPort_WaitUs(uint32_t us);
uint32_t TiaPort_IrqSave(void);
void     TiaPort_IrqRestore(uint32_t state);
void     TiaPort_SetTrigger(ADC_HandleTypeDef *hadc, DacBank_t bank);
#else
static void TiaPort_InitTiming(void)
{
    /* Contador de ciclos del DWT para esperas cortas en microsegundos */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void TiaPort_WaitUs(uint32_t us)
{
    uint32_t cycles = us * (SystemCoreClock / 1000000UL);
    uint32_t t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cycles) {
    }
}

static uint32_t TiaPort_IrqSave(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static void TiaPort_IrqRestore(uint32_t state)
{
    __set_PRIMASK(state);
}

/* Fuente de disparo del ADC: TRGO del timer del banco. Solo se puede
 * cambiar con el ADC sin convertir (ADSTART = 0), que es el caso aqui:
 * siempre se llama tras HAL_ADC_Stop_DMA o antes del primer arranque. */
static void TiaPort_SetTrigger(ADC_HandleTypeDef *hadc, DacBank_t bank)
{
    uint32_t sel = (bank == DAC_BANK_A) ? ADC_EXTERNALTRIG_T1_TRGO : ADC_EXTERNALTRIG_T8_TRGO;
    MODIFY_REG(hadc->Instance->CFGR, ADC_CFGR_EXTSEL, sel);
    hadc->Init.ExternalTrigConv = sel;
}
#endif /* TIA_READER_HOST_TEST */

/* ------------------------------------------------------------------------- *
 *  Estado
 * ------------------------------------------------------------------------- */
#define TIA_READER_ROTATION_LEN  (8U)
static const uint8_t kRotation[TIA_READER_ROTATION_LEN] = { 1U, 5U, 2U, 6U, 3U, 7U, 4U, 8U };

static ADC_HandleTypeDef *s_hadc;
static uint16_t s_buf[TIA_READER_BUF_SAMPLES];

static TiaWindow_t s_win;            /* lo escribe la ISR, lo vacia TakeWindow */

static volatile bool s_running;
static volatile bool s_paused;
static volatile bool s_armed;        /* rafaga solicitada y pendiente         */
static uint8_t   s_mask;
static uint8_t   s_ch;               /* canal en medida (1..8), 0 = ninguno   */
static TiaPlan_t s_plan;
static uint8_t   s_edge;             /* 0 = primera rafaga, 1 = segunda       */
static uint16_t  s_part_max;
static uint16_t  s_part_min;
static uint32_t  s_arm_tick;
static uint32_t  s_timeout_ms;
static uint32_t  s_last_arm_tick;

/* ------------------------------------------------------------------------- *
 *  Funciones puras
 * ------------------------------------------------------------------------- */
TiaPlan_t TiaReader_PlanFor(uint32_t freq_hz)
{
    TiaPlan_t p;
    if (freq_hz == 0U) {
        p.mode = TIA_PLAN_SKIP;
        p.samples = 0U;
        return p;
    }
    /* muestras en un periodo, redondeado hacia arriba, mas margen */
    uint32_t per = (TIA_READER_FS_HZ + freq_hz - 1U) / freq_hz;
    uint32_t n = per + TIA_READER_PERIOD_MARGIN;
    if (n <= TIA_READER_BUF_SAMPLES) {
        p.mode = TIA_PLAN_PERIOD;
        p.samples = (uint16_t)n;
    } else {
        p.mode = TIA_PLAN_TWO_EDGES;
        p.samples = (uint16_t)TIA_READER_EDGE_SAMPLES;
    }
    return p;
}

uint8_t TiaReader_NextChannel(uint8_t current, uint8_t mask)
{
    if (mask == 0U) {
        return 0U;
    }
    uint8_t pos = 0U;   /* posicion en la rotacion a partir de la que buscar */
    if (current != 0U) {
        for (uint8_t i = 0U; i < TIA_READER_ROTATION_LEN; i++) {
            if (kRotation[i] == current) {
                pos = (uint8_t)(i + 1U);
                break;
            }
        }
    }
    for (uint8_t k = 0U; k < TIA_READER_ROTATION_LEN; k++) {
        uint8_t ch = kRotation[(pos + k) % TIA_READER_ROTATION_LEN];
        if ((mask & (uint8_t)(1U << (ch - 1U))) != 0U) {
            return ch;
        }
    }
    return 0U;
}

static DacBank_t BankOf(uint8_t ch)
{
    return (ch <= 4U) ? DAC_BANK_A : DAC_BANK_B;
}

/* ------------------------------------------------------------------------- *
 *  Rafagas
 * ------------------------------------------------------------------------- */
/* Arma una rafaga del canal s_ch. 'switch_mux' solo en la primera rafaga
 * de cada canal. Devuelve false si el banco esta parado. */
static bool ArmBurst(bool switch_mux)
{
    DacBank_t bank = BankOf(s_ch);
    uint32_t f = DacWaveform_GetFrequency(bank);
    TiaPlan_t plan = TiaReader_PlanFor(f);
    if (plan.mode == TIA_PLAN_SKIP) {
        return false;
    }
    if (switch_mux) {
        (void)TiaMux_Select(s_ch);
        TiaPort_WaitUs(TIA_READER_SETTLE_US);
        s_edge = 0U;
    }
    s_plan = plan;

    TiaPort_SetTrigger(s_hadc, bank);
    s_armed = true;
    s_arm_tick = HAL_GetTick();
    s_timeout_ms = (2000UL + f - 1U) / f + TIA_READER_TIMEOUT_EXTRA_MS;   /* 2 periodos */
    if (HAL_ADC_Start_DMA(s_hadc, (uint32_t *)s_buf, plan.samples) != HAL_OK) {
        s_armed = false;
        return false;
    }
    return true;
}

/* Busca el siguiente canal medible y arma su primera rafaga. */
static void ArmNextChannel(void)
{
    for (uint8_t tries = 0U; tries < TIA_READER_ROTATION_LEN; tries++) {
        s_ch = TiaReader_NextChannel(s_ch, s_mask);
        if (s_ch == 0U) {
            return;
        }
        if (ArmBurst(true)) {
            return;
        }
    }
}

static void AbortBurst(void)
{
    if (s_armed) {
        (void)HAL_ADC_Stop_DMA(s_hadc);
        s_armed = false;
    }
    s_edge = 0U;
}

static void Accumulate(uint8_t ch, uint16_t vmax, uint16_t vmin)
{
    uint8_t i = (uint8_t)(ch - 1U);
    if (s_win.count[i] == 0xFFFFU) {
        return;   /* ventana saturada (no deberia pasar con ventanas de 1 s) */
    }
    s_win.sum_max[i] += vmax;
    s_win.sum_min[i] += vmin;
    s_win.count[i]++;
}

void TiaReader_OnBurstComplete(void)
{
    if (!s_armed) {
        return;
    }
    (void)HAL_ADC_Stop_DMA(s_hadc);
    s_armed = false;

    uint16_t vmax = 0U;
    uint16_t vmin = 0xFFFFU;
    for (uint16_t k = 0U; k < s_plan.samples; k++) {
        uint16_t v = s_buf[k];
        if (v > vmax) { vmax = v; }
        if (v < vmin) { vmin = v; }
    }

    if (s_plan.mode == TIA_PLAN_TWO_EDGES && s_edge == 0U) {
        /* Primera fase medida: se rearma enseguida para el flanco siguiente */
        s_part_max = vmax;
        s_part_min = vmin;
        s_edge = 1U;
        if (!s_paused && s_running) {
            if (!ArmBurst(false)) {
                s_edge = 0U;   /* el banco se ha parado: medida descartada */
            }
        }
        return;
    }

    if (s_plan.mode == TIA_PLAN_TWO_EDGES) {
        if (s_part_max > vmax) { vmax = s_part_max; }
        if (s_part_min < vmin) { vmin = s_part_min; }
        s_edge = 0U;
    }
    Accumulate(s_ch, vmax, vmin);
    /* El siguiente canal lo arma TiaReader_Task (como mucho uno por ms). */
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc == s_hadc) {
        TiaReader_OnBurstComplete();
    }
}

/* ------------------------------------------------------------------------- *
 *  API publica
 * ------------------------------------------------------------------------- */
HAL_StatusTypeDef TiaReader_Init(ADC_HandleTypeDef *hadc)
{
    if (hadc == NULL) {
        return HAL_ERROR;
    }
    s_hadc = hadc;
    s_running = false;
    s_paused = false;
    s_armed = false;
    s_ch = 0U;
    s_edge = 0U;
    memset(&s_win, 0, sizeof(s_win));
    TiaPort_InitTiming();
    TiaMux_Disable();

    /* Calibracion de offset del ADC (con el ADC deshabilitado) */
    return HAL_ADCEx_Calibration_Start(s_hadc, ADC_SINGLE_ENDED);
}

void TiaReader_Start(uint8_t channel_mask)
{
    uint32_t irq = TiaPort_IrqSave();
    AbortBurst();
    memset(&s_win, 0, sizeof(s_win));
    s_mask = channel_mask;
    s_ch = 0U;
    s_last_arm_tick = HAL_GetTick() - 1U;   /* permite armar en este mismo ms */
    s_paused = false;
    s_running = (channel_mask != 0U);
    TiaPort_IrqRestore(irq);
}

void TiaReader_Stop(void)
{
    uint32_t irq = TiaPort_IrqSave();
    s_running = false;
    AbortBurst();
    TiaPort_IrqRestore(irq);
    TiaMux_Disable();
}

void TiaReader_Pause(void)
{
    uint32_t irq = TiaPort_IrqSave();
    s_paused = true;
    AbortBurst();
    TiaPort_IrqRestore(irq);
}

void TiaReader_Resume(void)
{
    s_paused = false;   /* el rearme lo hace TiaReader_Task */
}

bool TiaReader_IsRunning(void)
{
    return s_running;
}

void TiaReader_TakeWindow(TiaWindow_t *out)
{
    if (out == NULL) {
        return;
    }
    uint32_t irq = TiaPort_IrqSave();
    memcpy(out, &s_win, sizeof(*out));
    memset(&s_win, 0, sizeof(s_win));
    TiaPort_IrqRestore(irq);
}

void TiaReader_Task(void)
{
    if (!s_running || s_paused) {
        return;
    }
    uint32_t now = HAL_GetTick();

    uint32_t irq = TiaPort_IrqSave();
    if (s_armed) {
        if ((now - s_arm_tick) > s_timeout_ms) {
            AbortBurst();   /* la rafaga no llega: canal sin dato, se sigue */
        }
        TiaPort_IrqRestore(irq);
        return;
    }
    if (s_edge != 0U) {
        /* rearme de la segunda fase fallido en la ISR: se descarta */
        s_edge = 0U;
    }
    TiaPort_IrqRestore(irq);

    /* Una medida nueva como mucho por milisegundo: ~1000 medidas/s, mas
     * que suficiente para promediar en ventanas de 1 s sin cargar la CPU
     * con interrupciones a la cadencia de la onda. */
    if (now == s_last_arm_tick) {
        return;
    }
    s_last_arm_tick = now;
    ArmNextChannel();
}
