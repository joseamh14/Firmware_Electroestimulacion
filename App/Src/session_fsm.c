/**
 ******************************************************************************
 * @file    session_fsm.c
 * @brief   Maquina de estados del protocolo autonomo de estimulacion.
 ******************************************************************************
 */
#include "session_fsm.h"
#include "flash_log.h"

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

/* ------------------------------------------------------------------------- *
 *  Copia de la sesion en flash interna (ver flash_log.h)
 *
 *  En esta placa VBAT esta unido a VDD: un corte de alimentacion borra los
 *  registros de backup Y reinicia el calendario del RTC. Por eso la sesion
 *  se guarda tambien en flash, con el tiempo de sesion ya transcurrido (no
 *  una marca de reloj) y un punto de control cada SESSION_FLASH_CKPT_S
 *  segundos. Tras un corte se reanuda desde el ultimo punto de control: se
 *  pierden como mucho SESSION_FLASH_CKPT_S segundos y el tiempo sin
 *  alimentacion no cuenta como tiempo de sesion.
 *
 *  Desgaste: con un punto por minuto y sectores de 8 KB en ping-pong
 *  (128 registros de 64 B por sector), cada sector se borra unas 6 veces
 *  al dia; muy por debajo de los 10 000 ciclos garantizados por ST en un
 *  ano de uso continuo (no verificado contra el datasheet del L4R5).
 * ------------------------------------------------------------------------- */
#define SESSION_FLASH_MAGIC       (0x53455346UL)   /* 'S''E''S''F' */
#define SESSION_FLASH_CKPT_S      (60UL)
#define SESSION_FLASH_WORDS       (13U)

#define SESSION_F_STATE           (0U)
#define SESSION_F_ELAPSED_S       (1U)
#define SESSION_F_ON_S            (2U)
#define SESSION_F_OFF_S           (3U)
#define SESSION_F_DURATION_S      (4U)
#define SESSION_F_FREQ_A          (5U)
#define SESSION_F_FREQ_B          (6U)
#define SESSION_F_AMP_12          (7U)
#define SESSION_F_AMP_34          (8U)
#define SESSION_F_AMP_56          (9U)
#define SESSION_F_AMP_78          (10U)
#define SESSION_F_WELL_MASK       (11U)
#define SESSION_F_RESERVED        (12U)

static const FlashLog_Config_t kSessionFlash = {
    { FLASH_LOG_SESSION_SECTOR_A, FLASH_LOG_SESSION_SECTOR_B },
    SESSION_FLASH_MAGIC,
    FLASH_LOG_RECORD_SIZE_FOR(SESSION_FLASH_WORDS * 4U)
};

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
static uint32_t s_last_ckpt_epoch;          /* ultimo punto de control flash */
static uint32_t s_state_start_epoch;        /* segundos RTC al entrar al estado */
static uint32_t s_on_seconds;

/* true si la amplitud del canal esta realmente escrita en el AD5144 durante
 * el STIM_ON en curso. Un pocillo solo se conecta si su canal tiene
 * amplitud > 0 Y esa amplitud se ha aplicado: con 0 mV el AD5144 en
 * ganancia minima no da 0 V (~35 mV de pico), y si la escritura I2C falla
 * el canal quedaria con una amplitud distinta de la pedida. */
static bool s_amp_applied[SESSION_NUM_CHANNELS];
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
                crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
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

static void PersistFlash(void)
{
    uint32_t w[SESSION_FLASH_WORDS];
    uint32_t now = RtcToEpoch();

    w[SESSION_F_STATE]      = s_armed ? (uint32_t)s_state : (uint32_t)SESSION_STATE_IDLE;
    w[SESSION_F_ELAPSED_S]  = (s_armed && now >= s_session_start_epoch)
                              ? (now - s_session_start_epoch) : 0U;
    w[SESSION_F_ON_S]       = s_on_seconds;
    w[SESSION_F_OFF_S]      = s_off_seconds;
    w[SESSION_F_DURATION_S] = s_duration_seconds;
    w[SESSION_F_FREQ_A]     = s_freq_hz[DAC_BANK_A];
    w[SESSION_F_FREQ_B]     = s_freq_hz[DAC_BANK_B];
    w[SESSION_F_AMP_12]     = (uint32_t)s_amp_mv[0] | ((uint32_t)s_amp_mv[1] << 16);
    w[SESSION_F_AMP_34]     = (uint32_t)s_amp_mv[2] | ((uint32_t)s_amp_mv[3] << 16);
    w[SESSION_F_AMP_56]     = (uint32_t)s_amp_mv[4] | ((uint32_t)s_amp_mv[5] << 16);
    w[SESSION_F_AMP_78]     = (uint32_t)s_amp_mv[6] | ((uint32_t)s_amp_mv[7] << 16);
    w[SESSION_F_WELL_MASK]  = s_well_mask;
    w[SESSION_F_RESERVED]   = 0U;

    /* Un fallo de escritura no detiene la estimulacion: la copia en flash
     * es una red de seguridad, no la fuente primaria del estado. */
    (void)FlashLog_Append(&kSessionFlash, w, sizeof(w));
    s_last_ckpt_epoch = now;
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

    PersistFlash();
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
                                       SESSION_CHANNEL_IN_PEAK_MV);
}

/* Apagado seguro: mismo estado de salida en IDLE y en STIM_REST. */
static HAL_StatusTypeDef ForceOutputsSafe(void)
{
    HAL_StatusTypeDef status = HAL_OK;
    HAL_StatusTypeDef s;

    /* Orden: primero se llevan los pocillos a GND (ADG1634, accion GPIO
     * inmediata y sin dependencia del bus I2C), despues se aparca el DAC en
     * VBIAS (Dif = 0) y por ultimo se lleva el AD5144 a ganancia minima. */
    s = CultureSelector_AllDisconnect();
    if (s != HAL_OK) { status = s; }
    for (uint8_t i = 0U; i < SESSION_NUM_CHANNELS; i++) {
        s_amp_applied[i] = false;
    }

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

    return status;
}

/* Un pocillo se conecta solo si esta en la mascara, su canal tiene amplitud
 * mayor que 0 y esa amplitud esta aplicada en el AD5144. */
static bool WellEnabled(uint8_t channel /* 1..8 */)
{
    uint8_t i = (uint8_t)(channel - 1U);
    return (((s_well_mask >> i) & 1U) != 0U) && (s_amp_mv[i] > 0U) && s_amp_applied[i];
}

static HAL_StatusTypeDef ApplyWell(uint8_t channel)
{
    return CultureSelector_SetWell(channel, WellEnabled(channel));
}

/* Vuelca todas las consignas vivas al hardware (entrada en STIM_ON). Las
 * amplitudes se escriben antes de conectar ningun pocillo; un canal cuya
 * escritura falle se queda desconectado. */
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
        s_amp_applied[c - 1U] = (s == HAL_OK);
        if (s != HAL_OK) { status = s; }
    }
    for (uint8_t w = 1U; w <= SESSION_NUM_CHANNELS; w++) {
        s = ApplyWell(w);
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
static HAL_StatusTypeDef SessionFSM_Evaluate(bool force_apply)
{
    HAL_StatusTypeDef status = HAL_OK;
    if (!s_armed) {
        return HAL_OK;
    }

    uint32_t now = RtcToEpoch();

    /* El reloj no puede ir hacia atras respecto al arranque de la sesion:
     * si pasa, el dominio de backup se ha perdido -> apagado seguro. */
    if (now < s_session_start_epoch) {
        (void)SessionFSM_Stop();
        return HAL_OK;
    }

    uint32_t total_elapsed = now - s_session_start_epoch;
    if (total_elapsed >= s_duration_seconds) {
        (void)SessionFSM_Stop();     /* sesion completada */
        return HAL_OK;
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
            status = ApplyStimOn();
        } else {
            status = ForceOutputsSafe();
        }
        PersistState();   /* incluye el punto de control en flash */
    } else if (now - s_last_ckpt_epoch >= SESSION_FLASH_CKPT_S) {
        PersistFlash();
    }
    return status;
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

/* Segunda opcion de recuperacion: la copia en flash. El instante de
 * arranque se recoloca de forma que el tiempo de sesion transcurrido sea
 * el del ultimo punto de control (el RTC se ha reiniciado con el corte). */
static bool TryRecoverFromFlash(void)
{
    uint32_t w[SESSION_FLASH_WORDS];
    if (FlashLog_ReadLatest(&kSessionFlash, w, sizeof(w), NULL) != HAL_OK) {
        return false;
    }

    uint32_t st = w[SESSION_F_STATE];
    if (st != SESSION_STATE_STIM_ON && st != SESSION_STATE_STIM_REST) {
        return false;   /* la ultima copia es de una sesion ya parada */
    }

    s_state               = (SessionState_t)st;
    s_on_seconds          = w[SESSION_F_ON_S];
    s_off_seconds         = w[SESSION_F_OFF_S];
    s_duration_seconds    = w[SESSION_F_DURATION_S];
    s_freq_hz[DAC_BANK_A] = w[SESSION_F_FREQ_A];
    s_freq_hz[DAC_BANK_B] = w[SESSION_F_FREQ_B];
    s_amp_mv[0] = (uint16_t)(w[SESSION_F_AMP_12] & 0xFFFFU);
    s_amp_mv[1] = (uint16_t)(w[SESSION_F_AMP_12] >> 16);
    s_amp_mv[2] = (uint16_t)(w[SESSION_F_AMP_34] & 0xFFFFU);
    s_amp_mv[3] = (uint16_t)(w[SESSION_F_AMP_34] >> 16);
    s_amp_mv[4] = (uint16_t)(w[SESSION_F_AMP_56] & 0xFFFFU);
    s_amp_mv[5] = (uint16_t)(w[SESSION_F_AMP_56] >> 16);
    s_amp_mv[6] = (uint16_t)(w[SESSION_F_AMP_78] & 0xFFFFU);
    s_amp_mv[7] = (uint16_t)(w[SESSION_F_AMP_78] >> 16);
    s_well_mask = (uint8_t)(w[SESSION_F_WELL_MASK] & 0xFFU);

    if (!ParamsPlausible()) {
        return false;
    }

    uint32_t elapsed = w[SESSION_F_ELAPSED_S];
    uint32_t now = RtcToEpoch();
    if (elapsed >= s_duration_seconds || now < elapsed) {
        return false;
    }
    s_session_start_epoch = now - elapsed;
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

    s_last_ckpt_epoch = RtcToEpoch();

    if (TryRecoverFromBackup()) {
        /* Reanuda: recoloca el estado segun el tiempo transcurrido real y
         * vuelca las consignas al hardware. */
        (void)SessionFSM_Evaluate(true);
    } else if (TryRecoverFromFlash()) {
        /* Corte con perdida del dominio de backup: se reanuda desde el
         * ultimo punto de control en flash. */
        (void)SessionFSM_Evaluate(true);
    } else {
        ClearBackup();
        EnterIdleSafe();
    }

    return HAL_OK;
}

bool SessionFSM_StartParamsValid(uint32_t on_seconds,
                                 uint32_t off_seconds,
                                 uint32_t duration_seconds)
{
    if (on_seconds < SESSION_ON_SECONDS_MIN || on_seconds > SESSION_ON_SECONDS_MAX) {
        return false;
    }
    if (off_seconds > SESSION_OFF_SECONDS_MAX) {
        return false;
    }
    if (duration_seconds < SESSION_DURATION_SECONDS_MIN ||
        duration_seconds > SESSION_DURATION_SECONDS_MAX) {
        return false;
    }
    if (duration_seconds < on_seconds) {
        return false;   /* no cabe ni un tramo de estimulacion completo */
    }
    return true;
}

HAL_StatusTypeDef SessionFSM_Start(uint32_t on_seconds,
                                   uint32_t off_seconds,
                                   uint32_t duration_seconds)
{
    if (!SessionFSM_StartParamsValid(on_seconds, off_seconds, duration_seconds)) {
        return HAL_ERROR;
    }

    s_on_seconds          = on_seconds;
    s_off_seconds         = off_seconds;
    s_duration_seconds    = duration_seconds;
    s_session_start_epoch = RtcToEpoch();
    s_state               = SESSION_STATE_STIM_ON;   /* provisional */
    s_armed               = true;

    /* Fija el estado real y aplica salidas. Si el hardware falla al armar
     * (bus I2C del AD5144, DAC), no se deja una sesion aplicada a medias:
     * apagado seguro, sesion cancelada y error al llamante. */
    HAL_StatusTypeDef st = SessionFSM_Evaluate(true);
    if (st != HAL_OK) {
        (void)SessionFSM_Stop();
        return st;
    }
    return HAL_OK;
}

HAL_StatusTypeDef SessionFSM_Stop(void)
{
    bool was_armed = s_armed;
    HAL_StatusTypeDef status = ForceOutputsSafe();

    s_state = SESSION_STATE_IDLE;
    s_armed = false;
    s_session_start_epoch = 0U;
    s_state_start_epoch = 0U;
    ClearBackup();
    if (was_armed) {
        PersistFlash();   /* registro IDLE: la sesion parada no debe resucitar */
    }

    return status;
}

void SessionFSM_Task(void)
{
    (void)SessionFSM_Evaluate(false);
}

HAL_StatusTypeDef SessionFSM_SetChannelMillivolts(uint8_t channel, uint16_t target_mv)
{
    if (channel < 1U || channel > SESSION_NUM_CHANNELS) {
        return HAL_ERROR;
    }
    if (target_mv > SESSION_AMP_MAX_MV) {
        return HAL_ERROR;
    }

    uint8_t  i    = (uint8_t)(channel - 1U);
    uint16_t prev = s_amp_mv[i];
    HAL_StatusTypeDef status = HAL_OK;

    if (s_state == SESSION_STATE_STIM_ON) {
        if (target_mv == 0U) {
            /* 0 mV: primero se desconecta el pocillo (a GND), luego el
             * AD5144 pasa a ganancia minima. */
            s_amp_mv[i] = 0U;
            (void)ApplyWell(channel);
            status = ApplyChannelAmplitude(channel);
            s_amp_applied[i] = (status == HAL_OK);
        } else {
            s_amp_mv[i] = target_mv;
            status = ApplyChannelAmplitude(channel);
            if (status != HAL_OK) {
                /* El AD5144 conserva el codigo anterior: se vuelve a la
                 * consigna anterior para no dar por aplicado lo que no lo
                 * esta, y no se persiste. */
                s_amp_mv[i] = prev;
                (void)ApplyWell(channel);
                return status;
            }
            s_amp_applied[i] = true;
            status = ApplyWell(channel);   /* conecta si estaba en la mascara */
        }
    } else {
        s_amp_mv[i] = target_mv;
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

    uint32_t prev = s_freq_hz[bank];
    s_freq_hz[bank] = freq_hz;

    HAL_StatusTypeDef status = HAL_OK;
    if (s_state == SESSION_STATE_STIM_ON) {
        status = DacWaveform_SetFrequency(bank, freq_hz);
        if (status != HAL_OK) {
            /* Se intenta recuperar la frecuencia anterior y no se persiste */
            s_freq_hz[bank] = prev;
            (void)DacWaveform_SetFrequency(bank, prev);
            return status;
        }
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
        status = ApplyWell(channel);   /* respeta la regla de amplitud > 0 */
    }
    if (s_armed) {
        PersistState();
    }
    return status;
}

HAL_StatusTypeDef SessionFSM_StoreAmplifierSafeDefault(void)
{
    if (s_state != SESSION_STATE_IDLE || s_armed) {
        return HAL_BUSY;
    }
    if (s_ad[0] == NULL || s_ad[1] == NULL) {
        return HAL_ERROR;
    }

    HAL_StatusTypeDef status = HAL_OK;
    HAL_StatusTypeDef s = AD5144_StoreSafePowerUp(s_ad[0]);
    if (s != HAL_OK) { status = s; }
    s = AD5144_StoreSafePowerUp(s_ad[1]);
    if (s != HAL_OK) { status = s; }
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
