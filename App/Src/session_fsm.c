/**
 ******************************************************************************
 * @file    session_fsm.c
 * @brief   Maquina de estados del protocolo autonomo de estimulacion.
 ******************************************************************************
 */
#include "session_fsm.h"

/* ------------------------------------------------------------------------- *
 *  Mapa de registros de backup del RTC
 *
 *  El bloque persistente ocupa RTC_BKP_DR0..RTC_BKP_DR12 (payload) mas
 *  RTC_BKP_DR13 (CRC-32 del payload). El STM32L4R5 tiene 32 registros de
 *  backup, asi que quedan de sobra para el logger u otros usos futuros.
 * ------------------------------------------------------------------------- */
#define SESSION_BKP_MAGIC_VALUE   (0x53455331UL)   /* 'S''E''S''1' */
#define SESSION_BKP_WORD_COUNT    (13U)            /* palabras de payload    */
#define SESSION_BKP_CRC_REG       (RTC_BKP_DR13)

/* Indices dentro del array de payload (y, por tanto, del array kBkpReg). */
#define SESSION_W_MAGIC           (0U)
#define SESSION_W_STATE           (1U)
#define SESSION_W_SESSION_TS      (2U)
#define SESSION_W_ON_S            (3U)
#define SESSION_W_OFF_S           (4U)
#define SESSION_W_DURATION_S      (5U)
#define SESSION_W_FREQ_A          (6U)
#define SESSION_W_FREQ_B          (7U)
#define SESSION_W_AMP_12          (8U)   /* canal 1 en [15:0], canal 2 en [31:16] */
#define SESSION_W_AMP_34          (9U)
#define SESSION_W_AMP_56          (10U)
#define SESSION_W_AMP_78          (11U)
#define SESSION_W_WELL_MASK       (12U)

static const uint32_t kBkpReg[SESSION_BKP_WORD_COUNT] = {
    RTC_BKP_DR0,  RTC_BKP_DR1,  RTC_BKP_DR2,  RTC_BKP_DR3,  RTC_BKP_DR4,
    RTC_BKP_DR5,  RTC_BKP_DR6,  RTC_BKP_DR7,  RTC_BKP_DR8,  RTC_BKP_DR9,
    RTC_BKP_DR10, RTC_BKP_DR11, RTC_BKP_DR12
};

/* ------------------------------------------------------------------------- *
 *  Estado interno
 * ------------------------------------------------------------------------- */
static RTC_HandleTypeDef *s_hrtc;
static AD5144_Handle_t   *s_ad[2];          /* [0] = canales 1-4, [1] = 5-8  */

static SessionState_t s_state = SESSION_STATE_IDLE;
static bool     s_armed;                    /* hay sesion en curso           */

static uint32_t s_session_start_epoch;      /* segundos RTC en el START      */
static uint32_t s_state_start_epoch;        /* segundos RTC al entrar al estado */
static uint32_t s_on_seconds;
static uint32_t s_off_seconds;
static uint32_t s_duration_seconds;

/* Consignas vivas (las fija el host con SET, se aplican en STIM_ON). */
static uint16_t s_amp_mv[SESSION_NUM_CHANNELS];
static uint32_t s_freq_hz[2];
static uint8_t  s_well_mask;

/* ------------------------------------------------------------------------- *
 *  Utilidades de tiempo (calendario RTC -> segundos monotonos)
 *
 *  Solo interesan DIFERENCIAS de tiempo, no la hora real: el origen del
 *  epoch es irrelevante mientras el RTC siga contando entre arranques.
 * ------------------------------------------------------------------------- */
static uint64_t CivilToDays(uint32_t y, uint32_t m, uint32_t d)
{
    /* Algoritmo de dias-desde-epoch de H. Hinnant (dominio publico). */
    y -= (m <= 2U);
    uint32_t era = y / 400U;
    uint32_t yoe = y - era * 400U;                         /* [0, 399]       */
    uint32_t mp  = (m > 2U) ? (m - 3U) : (m + 9U);         /* [0, 11]        */
    uint32_t doy = (153U * mp + 2U) / 5U + d - 1U;         /* [0, 365]       */
    uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (uint64_t)era * 146097U + (uint64_t)doe;        /* origen arbitrario */
}

static uint32_t RtcToEpoch(void)
{
    RTC_TimeTypeDef t  = {0};
    RTC_DateTypeDef dt = {0};

    /* GetDate es obligatorio tras GetTime aunque no se use: desbloquea las
     * copias sombra de los registros del RTC. */
    (void)HAL_RTC_GetTime(s_hrtc, &t, RTC_FORMAT_BIN);
    (void)HAL_RTC_GetDate(s_hrtc, &dt, RTC_FORMAT_BIN);

    uint64_t days = CivilToDays(2000U + dt.Year, dt.Month, dt.Date);
    uint64_t secs = days * SESSION_SECONDS_PER_DAY
                  + (uint64_t)t.Hours   * SESSION_SECONDS_PER_HOUR
                  + (uint64_t)t.Minutes * SESSION_SECONDS_PER_MINUTE
                  + (uint64_t)t.Seconds;
    return (uint32_t)secs;
}

/* ------------------------------------------------------------------------- *
 *  Persistencia en backup
 * ------------------------------------------------------------------------- */
static uint32_t Crc32(const uint32_t *words, uint32_t count)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t w = words[i];
        for (uint8_t byte = 0U; byte < 4U; byte++) {
            crc ^= (w & 0xFFU);
            w >>= 8;
            for (uint8_t k = 0U; k < 8U; k++) {
                crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88520UL) : (crc >> 1);
            }
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

static void PackPayload(uint32_t *w)
{
    w[SESSION_W_MAGIC]      = SESSION_BKP_MAGIC_VALUE;
    w[SESSION_W_STATE]      = (uint32_t)s_state;
    w[SESSION_W_SESSION_TS] = s_session_start_epoch;
    w[SESSION_W_ON_S]       = s_on_seconds;
    w[SESSION_W_OFF_S]      = s_off_seconds;
    w[SESSION_W_DURATION_S] = s_duration_seconds;
    w[SESSION_W_FREQ_A]     = s_freq_hz[DAC_BANK_A];
    w[SESSION_W_FREQ_B]     = s_freq_hz[DAC_BANK_B];
    w[SESSION_W_AMP_12]     = (uint32_t)s_amp_mv[0] | ((uint32_t)s_amp_mv[1] << 16);
    w[SESSION_W_AMP_34]     = (uint32_t)s_amp_mv[2] | ((uint32_t)s_amp_mv[3] << 16);
    w[SESSION_W_AMP_56]     = (uint32_t)s_amp_mv[4] | ((uint32_t)s_amp_mv[5] << 16);
    w[SESSION_W_AMP_78]     = (uint32_t)s_amp_mv[6] | ((uint32_t)s_amp_mv[7] << 16);
    w[SESSION_W_WELL_MASK]  = s_well_mask;
}

static void PersistState(void)
{
    uint32_t w[SESSION_BKP_WORD_COUNT];
    PackPayload(w);
    uint32_t crc = Crc32(w, SESSION_BKP_WORD_COUNT);

    for (uint32_t i = 0U; i < SESSION_BKP_WORD_COUNT; i++) {
        HAL_RTCEx_BKUPWrite(s_hrtc, kBkpReg[i], w[i]);
    }
    HAL_RTCEx_BKUPWrite(s_hrtc, SESSION_BKP_CRC_REG, crc);
}

static void ClearBackup(void)
{
    HAL_RTCEx_BKUPWrite(s_hrtc, kBkpReg[SESSION_W_MAGIC], 0U);
    HAL_RTCEx_BKUPWrite(s_hrtc, SESSION_BKP_CRC_REG, 0U);
}

/* ------------------------------------------------------------------------- *
 *  Aplicacion de salidas
 * ------------------------------------------------------------------------- */
static HAL_StatusTypeDef ApplyChannelAmplitude(uint8_t channel /* 1..8 */)
{
    AD5144_Handle_t *dev = (channel <= 4U) ? s_ad[0] : s_ad[1];
    AD5144_Channel_t sub = (AD5144_Channel_t)((channel <= 4U) ? (channel - 1U)
                                                              : (channel - 5U));
    return AD5144_SetChannelMillivolts(dev, sub, s_amp_mv[channel - 1U],
                                       SESSION_CHANNEL_FULL_SCALE_MV);
}

/* Apagado seguro: mismo estado de salida en IDLE y en STIM_REST. */
static HAL_StatusTypeDef ForceOutputsSafe(void)
{
    HAL_StatusTypeDef status = HAL_OK;
    HAL_StatusTypeDef s;

    s = DacWaveform_Stop(DAC_BANK_A);
    if (s != HAL_OK) { status = s; }
    s = DacWaveform_Stop(DAC_BANK_B);
    if (s != HAL_OK) { status = s; }

    if (s_ad[0] != NULL) {
        s = AD5144_AllChannelsZero(s_ad[0]);
        if (s != HAL_OK) { status = s; }
    }
    if (s_ad[1] != NULL) {
        s = AD5144_AllChannelsZero(s_ad[1]);
        if (s != HAL_OK) { status = s; }
    }

    s = CultureSelector_AllDisconnect();
    if (s != HAL_OK) { status = s; }

    return status;
}

/* Vuelca todas las consignas vivas al hardware (entrada en STIM_ON). */
static HAL_StatusTypeDef ApplyStimOn(void)
{
    HAL_StatusTypeDef status = HAL_OK;
    HAL_StatusTypeDef s;

    /* freq_hz == 0 -> DacWaveform_SetFrequency detiene el banco (HAL_OK). */
    s = DacWaveform_SetFrequency(DAC_BANK_A, s_freq_hz[DAC_BANK_A]);
    if (s != HAL_OK) { status = s; }
    s = DacWaveform_SetFrequency(DAC_BANK_B, s_freq_hz[DAC_BANK_B]);
    if (s != HAL_OK) { status = s; }

    for (uint8_t c = 1U; c <= SESSION_NUM_CHANNELS; c++) {
        s = ApplyChannelAmplitude(c);
        if (s != HAL_OK) { status = s; }
    }
    for (uint8_t w = 1U; w <= SESSION_NUM_CHANNELS; w++) {
        bool connected = ((s_well_mask >> (w - 1U)) & 1U) != 0U;
        s = CultureSelector_SetWell(w, connected);
        if (s != HAL_OK) { status = s; }
    }

    return status;
}

static void EnterIdleSafe(void)
{
    (void)ForceOutputsSafe();
    s_state = SESSION_STATE_IDLE;
    s_armed = false;
    s_state_start_epoch = 0U;
}

/* ------------------------------------------------------------------------- *
 *  Motor de la FSM
 *
 *  En vez de contar transiciones, se recalcula en cada evaluacion en que
 *  punto del ciclo ON/REST toca estar segun el tiempo transcurrido desde
 *  el START. Asi la reanudacion tras un corte de cualquier duracion es
 *  automatica y no acumula deriva.
 * ------------------------------------------------------------------------- */
static void SessionFSM_Evaluate(bool force_apply)
{
    if (!s_armed) {
        return;
    }

    uint32_t now = RtcToEpoch();

    /* El reloj no puede ir hacia atras respecto al arranque de la sesion:
     * si pasa, el dominio de backup se ha perdido -> apagado seguro. */
    if (now < s_session_start_epoch) {
        (void)SessionFSM_Stop();
        return;
    }

    uint32_t total_elapsed = now - s_session_start_epoch;
    if (total_elapsed >= s_duration_seconds) {
        (void)SessionFSM_Stop();     /* sesion completada */
        return;
    }

    SessionState_t desired;
    if (s_off_seconds == 0U) {
        desired = SESSION_STATE_STIM_ON;
        s_state_start_epoch = now - (total_elapsed % s_on_seconds);
    } else {
        uint32_t cycle = s_on_seconds + s_off_seconds;
        uint32_t rem   = total_elapsed % cycle;
        if (rem < s_on_seconds) {
            desired = SESSION_STATE_STIM_ON;
            s_state_start_epoch = now - rem;
        } else {
            desired = SESSION_STATE_STIM_REST;
            s_state_start_epoch = now - (rem - s_on_seconds);
        }
    }

    if (force_apply || desired != s_state) {
        s_state = desired;
        if (desired == SESSION_STATE_STIM_ON) {
            (void)ApplyStimOn();
        } else {
            (void)ForceOutputsSafe();
        }
        PersistState();
    }
}

/* ------------------------------------------------------------------------- *
 *  Recuperacion de sesion desde backup
 * ------------------------------------------------------------------------- */
static bool ParamsPlausible(void)
{
    if (s_on_seconds < SESSION_ON_SECONDS_MIN || s_on_seconds > SESSION_ON_SECONDS_MAX) {
        return false;
    }
    if (s_off_seconds > SESSION_OFF_SECONDS_MAX) {
        return false;
    }
    if (s_duration_seconds < SESSION_DURATION_SECONDS_MIN ||
        s_duration_seconds > SESSION_DURATION_SECONDS_MAX) {
        return false;
    }
    if (s_duration_seconds < s_on_seconds) {
        return false;
    }
    for (uint8_t i = 0U; i < SESSION_NUM_CHANNELS; i++) {
        if (s_amp_mv[i] > SESSION_AMP_MAX_MV) {
            return false;
        }
    }
    if (s_freq_hz[DAC_BANK_A] > DAC_FREQ_MAX_HZ ||
        s_freq_hz[DAC_BANK_B] > DAC_FREQ_MAX_HZ) {
        return false;
    }
    return true;
}

static bool TryRecoverFromBackup(void)
{
    uint32_t w[SESSION_BKP_WORD_COUNT];
    for (uint32_t i = 0U; i < SESSION_BKP_WORD_COUNT; i++) {
        w[i] = HAL_RTCEx_BKUPRead(s_hrtc, kBkpReg[i]);
    }

    if (w[SESSION_W_MAGIC] != SESSION_BKP_MAGIC_VALUE) {
        return false;
    }
    if (Crc32(w, SESSION_BKP_WORD_COUNT) != HAL_RTCEx_BKUPRead(s_hrtc, SESSION_BKP_CRC_REG)) {
        return false;
    }

    uint32_t st = w[SESSION_W_STATE];
    if (st != SESSION_STATE_STIM_ON && st != SESSION_STATE_STIM_REST) {
        return false;   /* IDLE guardado o valor imposible: nada que reanudar */
    }

    s_state               = (SessionState_t)st;
    s_session_start_epoch = w[SESSION_W_SESSION_TS];
    s_on_seconds          = w[SESSION_W_ON_S];
    s_off_seconds         = w[SESSION_W_OFF_S];
    s_duration_seconds    = w[SESSION_W_DURATION_S];
    s_freq_hz[DAC_BANK_A] = w[SESSION_W_FREQ_A];
    s_freq_hz[DAC_BANK_B] = w[SESSION_W_FREQ_B];
    s_amp_mv[0] = (uint16_t)(w[SESSION_W_AMP_12] & 0xFFFFU);
    s_amp_mv[1] = (uint16_t)(w[SESSION_W_AMP_12] >> 16);
    s_amp_mv[2] = (uint16_t)(w[SESSION_W_AMP_34] & 0xFFFFU);
    s_amp_mv[3] = (uint16_t)(w[SESSION_W_AMP_34] >> 16);
    s_amp_mv[4] = (uint16_t)(w[SESSION_W_AMP_56] & 0xFFFFU);
    s_amp_mv[5] = (uint16_t)(w[SESSION_W_AMP_56] >> 16);
    s_amp_mv[6] = (uint16_t)(w[SESSION_W_AMP_78] & 0xFFFFU);
    s_amp_mv[7] = (uint16_t)(w[SESSION_W_AMP_78] >> 16);
    s_well_mask = (uint8_t)(w[SESSION_W_WELL_MASK] & 0xFFU);

    if (!ParamsPlausible()) {
        return false;
    }

    s_armed = true;
    return true;
}

/* ------------------------------------------------------------------------- *
 *  API publica
 * ------------------------------------------------------------------------- */
HAL_StatusTypeDef SessionFSM_Init(RTC_HandleTypeDef *hrtc,
                                  AD5144_Handle_t *ad_bank_a,
                                  AD5144_Handle_t *ad_bank_b)
{
    if (hrtc == NULL || ad_bank_a == NULL || ad_bank_b == NULL) {
        return HAL_ERROR;
    }

    s_hrtc  = hrtc;
    s_ad[0] = ad_bank_a;
    s_ad[1] = ad_bank_b;

    /* Acceso de escritura al dominio de backup (RTC->BKPxR). Idempotente. */
    HAL_PWR_EnableBkUpAccess();

    /* Consignas por defecto: todo a 0 hasta que el host las fije. */
    for (uint8_t i = 0U; i < SESSION_NUM_CHANNELS; i++) {
        s_amp_mv[i] = 0U;
    }
    s_freq_hz[DAC_BANK_A] = 0U;
    s_freq_hz[DAC_BANK_B] = 0U;
    s_well_mask = 0U;
    s_on_seconds = 0U;
    s_off_seconds = 0U;
    s_duration_seconds = 0U;
    s_session_start_epoch = 0U;
    s_state_start_epoch = 0U;
    s_state = SESSION_STATE_IDLE;
    s_armed = false;

    if (TryRecoverFromBackup()) {
        /* Reanuda: recoloca el estado segun el tiempo transcurrido real y
         * vuelca las consignas al hardware. */
        SessionFSM_Evaluate(true);
    } else {
        ClearBackup();
        EnterIdleSafe();
    }

    return HAL_OK;
}

HAL_StatusTypeDef SessionFSM_Start(uint32_t on_seconds,
                                   uint32_t off_seconds,
                                   uint32_t duration_seconds)
{
    if (on_seconds < SESSION_ON_SECONDS_MIN || on_seconds > SESSION_ON_SECONDS_MAX) {
        return HAL_ERROR;
    }
    if (off_seconds > SESSION_OFF_SECONDS_MAX) {
        return HAL_ERROR;
    }
    if (duration_seconds < SESSION_DURATION_SECONDS_MIN ||
        duration_seconds > SESSION_DURATION_SECONDS_MAX) {
        return HAL_ERROR;
    }
    if (duration_seconds < on_seconds) {
        return HAL_ERROR;   /* no cabe ni un tramo de estimulacion completo */
    }

    s_on_seconds          = on_seconds;
    s_off_seconds         = off_seconds;
    s_duration_seconds    = duration_seconds;
    s_session_start_epoch = RtcToEpoch();
    s_state               = SESSION_STATE_STIM_ON;   /* provisional */
    s_armed               = true;

    SessionFSM_Evaluate(true);   /* fija el estado real y aplica salidas */
    return HAL_OK;
}

HAL_StatusTypeDef SessionFSM_Stop(void)
{
    HAL_StatusTypeDef status = ForceOutputsSafe();

    s_state = SESSION_STATE_IDLE;
    s_armed = false;
    s_session_start_epoch = 0U;
    s_state_start_epoch = 0U;
    ClearBackup();

    return status;
}

void SessionFSM_Task(void)
{
    SessionFSM_Evaluate(false);
}

HAL_StatusTypeDef SessionFSM_SetChannelMillivolts(uint8_t channel, uint16_t target_mv)
{
    if (channel < 1U || channel > SESSION_NUM_CHANNELS) {
        return HAL_ERROR;
    }
    if (target_mv > SESSION_AMP_MAX_MV) {
        return HAL_ERROR;
    }

    s_amp_mv[channel - 1U] = target_mv;

    HAL_StatusTypeDef status = HAL_OK;
    if (s_state == SESSION_STATE_STIM_ON) {
        status = ApplyChannelAmplitude(channel);
    }
    if (s_armed) {
        PersistState();
    }
    return status;
}

HAL_StatusTypeDef SessionFSM_SetBankFrequency(DacBank_t bank, uint32_t freq_hz)
{
    if (bank != DAC_BANK_A && bank != DAC_BANK_B) {
        return HAL_ERROR;
    }
    if (freq_hz < DAC_FREQ_MIN_HZ || freq_hz > DAC_FREQ_MAX_HZ) {
        return HAL_ERROR;
    }

    s_freq_hz[bank] = freq_hz;

    HAL_StatusTypeDef status = HAL_OK;
    if (s_state == SESSION_STATE_STIM_ON) {
        status = DacWaveform_SetFrequency(bank, freq_hz);
    }
    if (s_armed) {
        PersistState();
    }
    return status;
}

HAL_StatusTypeDef SessionFSM_SetWell(uint8_t channel, bool connected)
{
    if (channel < 1U || channel > SESSION_NUM_CHANNELS) {
        return HAL_ERROR;
    }

    if (connected) {
        s_well_mask |= (uint8_t)(1U << (channel - 1U));
    } else {
        s_well_mask &= (uint8_t)~(1U << (channel - 1U));
    }

    HAL_StatusTypeDef status = HAL_OK;
    if (s_state == SESSION_STATE_STIM_ON) {
        status = CultureSelector_SetWell(channel, connected);
    }
    if (s_armed) {
        PersistState();
    }
    return status;
}

SessionState_t SessionFSM_GetState(void)
{
    return s_state;
}

void SessionFSM_GetStatus(SessionStatus_t *out)
{
    if (out == NULL) {
        return;
    }

    out->state            = s_state;
    out->on_seconds       = s_on_seconds;
    out->off_seconds      = s_off_seconds;
    out->duration_seconds = s_duration_seconds;
    out->freq_hz[0]       = s_freq_hz[DAC_BANK_A];
    out->freq_hz[1]       = s_freq_hz[DAC_BANK_B];
    out->well_mask        = s_well_mask;
    for (uint8_t i = 0U; i < SESSION_NUM_CHANNELS; i++) {
        out->amp_mv[i] = s_amp_mv[i];
    }

    out->elapsed_total_s    = 0U;
    out->elapsed_in_state_s = 0U;
    if (s_armed) {
        uint32_t now = RtcToEpoch();
        if (now >= s_session_start_epoch) {
            out->elapsed_total_s = now - s_session_start_epoch;
        }
        if (now >= s_state_start_epoch) {
            out->elapsed_in_state_s = now - s_state_start_epoch;
        }
    }
}
