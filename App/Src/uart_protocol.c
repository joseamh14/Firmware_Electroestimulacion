/**
 ******************************************************************************
 * @file    uart_protocol.c
 * @brief   Parser de comandos de texto del host sobre USB CDC.
 ******************************************************************************
 */
#include "uart_protocol.h"
#include "session_fsm.h"
#include "current_calc.h"
#include "sd_logger.h"
#include "usbd_cdc_if.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* Handle del dispositivo USB (definido en usb_device.c). Se consulta solo
 * para saber si la pila esta configurada antes de intentar transmitir. */
extern USBD_HandleTypeDef hUsbDeviceFS;

/* ------------------------------------------------------------------------- *
 *  Parametros de compilacion
 * ------------------------------------------------------------------------- */
#define UART_RX_RING_SIZE      (256U)   /* potencia de 2 (mascara)          */
#define UART_RX_RING_MASK      (UART_RX_RING_SIZE - 1U)
#define UART_LINE_MAX          (64U)    /* longitud maxima de comando       */
#define UART_MAX_TOKENS        (5U)     /* el comando mas largo (SET CAL) usa 5 */
#define UART_TX_BUF_SIZE       (320U)   /* la linea de STATUS cabe de sobra */
#define UART_TX_SPIN_LIMIT     (200000UL) /* techo de espera de la pila USB */
#define UART_DEC_MAX_INT       (100000UL) /* parte entera maxima de un decimal */
#define UART_DEC_MAX_FRAC_DIV  (1000000UL) /* hasta 6 decimales             */
#define UART_GAIN_DECIMALS     (6U)
#define UART_OFFSET_DECIMALS   (3U)

/* ------------------------------------------------------------------------- *
 *  Estado interno
 * ------------------------------------------------------------------------- */
/* Buffer circular productor(ISR) / consumidor(bucle) sin cerrojo:
 * head lo escribe solo UartProtocol_RxData, tail solo UartProtocol_Task. */
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;
static uint8_t           s_rx_ring[UART_RX_RING_SIZE];

static char     s_line[UART_LINE_MAX + 1U];
static uint16_t s_line_len;
static bool     s_line_overflow;

static uint8_t  s_tx[UART_TX_BUF_SIZE];

/* ------------------------------------------------------------------------- *
 *  Transmision
 * ------------------------------------------------------------------------- */
static void UartTx(const uint8_t *buf, uint16_t len)
{
    if (len == 0U || hUsbDeviceFS.pClassData == NULL) {
        return;   /* pila USB no configurada todavia: se descarta la respuesta */
    }

    uint32_t spin = UART_TX_SPIN_LIMIT;
    while (spin != 0U) {
        if (CDC_Transmit_FS((uint8_t *)buf, len) != (uint8_t)USBD_BUSY) {
            break;
        }
        spin--;
    }

    /* Esperar a que el paquete salga: s_tx es estatico y se reutiliza en la
     * siguiente respuesta, no puede reescribirse con el envio en curso. */
    USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
    spin = UART_TX_SPIN_LIMIT;
    while (hcdc != NULL && hcdc->TxState != 0U && spin != 0U) {
        spin--;
    }
}

static void SendOk(void)
{
    static const uint8_t kOk[] = { 'O', 'K', '\n' };
    memcpy(s_tx, kOk, sizeof(kOk));
    UartTx(s_tx, (uint16_t)sizeof(kOk));
}

static void SendErr(const char *reason)
{
    int n = snprintf((char *)s_tx, sizeof(s_tx), "ERR %s\n", reason);
    if (n > 0) {
        uint16_t len = (n >= (int)sizeof(s_tx)) ? (uint16_t)(sizeof(s_tx) - 1U)
                                                : (uint16_t)n;
        UartTx(s_tx, len);
    }
}

static const char *StateName(SessionState_t st)
{
    switch (st) {
        case SESSION_STATE_STIM_ON:   return "STIM_ON";
        case SESSION_STATE_STIM_REST: return "STIM_REST";
        case SESSION_STATE_IDLE:
        default:                      return "IDLE";
    }
}

static void SendStatus(void)
{
    SessionStatus_t st;
    SessionFSM_GetStatus(&st);

    int n = snprintf((char *)s_tx, sizeof(s_tx),
        "STATE=%s T_STATE=%lu T_TOTAL=%lu ON=%lu OFF=%lu DUR=%lu "
        "FREQ_A=%lu FREQ_B=%lu AMP=%u,%u,%u,%u,%u,%u,%u,%u WELL=0x%02X\n",
        StateName(st.state),
        (unsigned long)st.elapsed_in_state_s,
        (unsigned long)st.elapsed_total_s,
        (unsigned long)st.on_seconds,
        (unsigned long)st.off_seconds,
        (unsigned long)st.duration_seconds,
        (unsigned long)st.freq_hz[0],
        (unsigned long)st.freq_hz[1],
        (unsigned)st.amp_mv[0], (unsigned)st.amp_mv[1],
        (unsigned)st.amp_mv[2], (unsigned)st.amp_mv[3],
        (unsigned)st.amp_mv[4], (unsigned)st.amp_mv[5],
        (unsigned)st.amp_mv[6], (unsigned)st.amp_mv[7],
        (unsigned)st.well_mask);

    if (n > 0) {
        uint16_t len = (n >= (int)sizeof(s_tx)) ? (uint16_t)(sizeof(s_tx) - 1U)
                                                : (uint16_t)n;
        UartTx(s_tx, len);
    }
    SendOk();
}

/* ------------------------------------------------------------------------- *
 *  Utilidades de parseo
 * ------------------------------------------------------------------------- */
static bool StrEqualCI(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') { ca = (char)(ca - ('a' - 'A')); }
        if (cb >= 'a' && cb <= 'z') { cb = (char)(cb - ('a' - 'A')); }
        if (ca != cb) { return false; }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* Convierte una cadena de solo digitos a uint32_t. Rechaza cadena vacia,
 * cualquier caracter no numerico y el desbordamiento. */
static bool ParseU32(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    uint32_t v = 0U;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        uint32_t d = (uint32_t)(*p - '0');
        if (v > (0xFFFFFFFFUL - d) / 10U) {
            return false;   /* desbordamiento */
        }
        v = v * 10U + d;
    }
    *out = v;
    return true;
}

/* Trocea la linea en tokens separados por espacios/tabuladores (in situ).
 * Devuelve el numero de tokens, o -1 si hay mas de max. */
static int Tokenize(char *line, char **argv, uint32_t max)
{
    uint32_t n = 0U;
    char *p = line;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') { p++; }
        if (*p == '\0') { break; }
        if (n >= max) { return -1; }
        argv[n] = p;
        n++;
        while (*p != '\0' && *p != ' ' && *p != '\t') { p++; }
        if (*p != '\0') {
            *p = '\0';
            p++;
        }
    }
    return (int)n;
}

/* Convierte un decimal con signo opcional ("-12.345", "1.0021", "7") a
 * float, sin depender de strtof. Separador decimal: punto. Hasta 6
 * decimales y parte entera < 100000; cualquier otra cosa es error. */
static bool ParseDecimal(const char *s, float *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    const char *p = s;
    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }
    uint32_t ip = 0U;
    uint32_t fp = 0U;
    uint32_t fdiv = 1U;
    bool dot = false;
    bool any_digit = false;
    for (; *p != '\0'; p++) {
        if (*p == '.') {
            if (dot) {
                return false;
            }
            dot = true;
            continue;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        any_digit = true;
        uint32_t d = (uint32_t)(*p - '0');
        if (!dot) {
            if (ip >= UART_DEC_MAX_INT) {
                return false;
            }
            ip = ip * 10U + d;
        } else {
            if (fdiv >= UART_DEC_MAX_FRAC_DIV) {
                return false;
            }
            fp = fp * 10U + d;
            fdiv *= 10U;
        }
    }
    if (!any_digit) {
        return false;
    }
    float v = (float)ip + (float)fp / (float)fdiv;
    *out = neg ? -v : v;
    return true;
}

/* Escribe v con 'decimals' cifras decimales sin usar printf de coma
 * flotante (no enlazado en newlib-nano por defecto). */
static void FormatFixed(char *buf, size_t size, float v, uint32_t decimals)
{
    if (decimals > 9U) {
        decimals = 9U;
    }
    uint32_t pow10 = 1U;
    for (uint32_t i = 0U; i < decimals; i++) {
        pow10 *= 10U;
    }
    bool neg = (v < 0.0f);
    float a = neg ? -v : v;
    uint64_t scaled = (uint64_t)(a * (float)pow10 + 0.5f);
    unsigned long ip = (unsigned long)(scaled / pow10);
    uint32_t fp = (uint32_t)(scaled % pow10);

    /* Cifras decimales a mano (con ceros a la izquierda), sin depender de
     * que el printf de newlib-nano admita la anchura con '*'. */
    char frac[12];
    for (uint32_t i = decimals; i > 0U; i--) {
        frac[i - 1U] = (char)('0' + (fp % 10U));
        fp /= 10U;
    }
    frac[decimals] = '\0';
    (void)snprintf(buf, size, "%s%lu.%s", neg ? "-" : "", ip, frac);
}

/* ------------------------------------------------------------------------- *
 *  Interpretacion de comandos
 * ------------------------------------------------------------------------- */
static void HandleSetAmp(int argc, char **argv)
{
    uint32_t channel;
    uint32_t millivolts;
    if (argc != 4) {
        SendErr("SYNTAX");
        return;
    }
    if (!ParseU32(argv[2], &channel) || !ParseU32(argv[3], &millivolts)) {
        SendErr("SYNTAX");
        return;
    }
    if (channel < 1U || channel > SESSION_NUM_CHANNELS ||
        millivolts > SESSION_AMP_MAX_MV) {
        SendErr("RANGE");
        return;
    }
    if (SessionFSM_SetChannelMillivolts((uint8_t)channel, (uint16_t)millivolts) != HAL_OK) {
        SendErr("HW");
        return;
    }
    SendOk();
}

static void HandleSetFreq(int argc, char **argv)
{
    uint32_t freq_hz;
    DacBank_t bank;
    if (argc != 4) {
        SendErr("SYNTAX");
        return;
    }
    if (StrEqualCI(argv[2], "A")) {
        bank = DAC_BANK_A;
    } else if (StrEqualCI(argv[2], "B")) {
        bank = DAC_BANK_B;
    } else {
        SendErr("SYNTAX");
        return;
    }
    if (!ParseU32(argv[3], &freq_hz)) {
        SendErr("SYNTAX");
        return;
    }
    if (freq_hz < DAC_FREQ_MIN_HZ || freq_hz > DAC_FREQ_MAX_HZ) {
        SendErr("RANGE");
        return;
    }
    if (SessionFSM_SetBankFrequency(bank, freq_hz) != HAL_OK) {
        SendErr("HW");
        return;
    }
    SendOk();
}

static void HandleSetWell(int argc, char **argv)
{
    uint32_t channel;
    bool connected;
    if (argc != 4) {
        SendErr("SYNTAX");
        return;
    }
    if (!ParseU32(argv[2], &channel)) {
        SendErr("SYNTAX");
        return;
    }
    if (StrEqualCI(argv[3], "ON")) {
        connected = true;
    } else if (StrEqualCI(argv[3], "OFF")) {
        connected = false;
    } else {
        SendErr("SYNTAX");
        return;
    }
    if (channel < 1U || channel > SESSION_NUM_CHANNELS) {
        SendErr("RANGE");
        return;
    }
    if (SessionFSM_SetWell((uint8_t)channel, connected) != HAL_OK) {
        SendErr("HW");
        return;
    }
    SendOk();
}

static void HandleStart(int argc, char **argv)
{
    uint32_t on_hours;
    uint32_t off_hours;
    uint32_t days;
    if (argc != 4) {
        SendErr("SYNTAX");
        return;
    }
    if (!ParseU32(argv[1], &on_hours) ||
        !ParseU32(argv[2], &off_hours) ||
        !ParseU32(argv[3], &days)) {
        SendErr("SYNTAX");
        return;
    }
    if (on_hours  < SESSION_ON_HOURS_MIN  || on_hours  > SESSION_ON_HOURS_MAX ||
        off_hours < SESSION_OFF_HOURS_MIN || off_hours > SESSION_OFF_HOURS_MAX ||
        days      < SESSION_DAYS_MIN      || days      > SESSION_DAYS_MAX) {
        SendErr("RANGE");
        return;
    }

    uint32_t on_s  = on_hours  * SESSION_SECONDS_PER_HOUR;
    uint32_t off_s = off_hours * SESSION_SECONDS_PER_HOUR;
    uint32_t dur_s = days      * SESSION_SECONDS_PER_DAY;

    /* Combinacion incoherente (duracion total menor que un tramo de
     * estimulacion completo) -> rango; fallo al aplicar -> hardware. */
    if (!SessionFSM_StartParamsValid(on_s, off_s, dur_s)) {
        SendErr("RANGE");
        return;
    }
    if (SessionFSM_Start(on_s, off_s, dur_s) != HAL_OK) {
        SendErr("HW");
        return;
    }
    SendOk();
}

static void HandleSetCal(int argc, char **argv)
{
    uint32_t channel;
    float gain;
    float offset_ua;
    if (argc != 5) {
        SendErr("SYNTAX");
        return;
    }
    if (!ParseU32(argv[2], &channel) || !ParseDecimal(argv[3], &gain) ||
        !ParseDecimal(argv[4], &offset_ua)) {
        SendErr("SYNTAX");
        return;
    }
    if (channel < 1U || channel > CURRENT_CALC_NUM_CH) {
        SendErr("RANGE");
        return;
    }
    if (CurrentCalc_SetCalibration((uint8_t)channel, gain, offset_ua) != HAL_OK) {
        SendErr("RANGE");   /* ganancia u offset fuera de los limites */
        return;
    }
    SendOk();
}

static void HandleSetVref(int argc, char **argv)
{
    uint32_t vref_mv;
    if (argc != 3) {
        SendErr("SYNTAX");
        return;
    }
    if (!ParseU32(argv[2], &vref_mv)) {
        SendErr("SYNTAX");
        return;
    }
    if (vref_mv > 0xFFFFU || CurrentCalc_SetVrefMillivolts((uint16_t)vref_mv) != HAL_OK) {
        SendErr("RANGE");
        return;
    }
    SendOk();
}

static void HandleGetCal(int argc, char **argv)
{
    uint32_t channel;
    CurrentCalc_Cal_t cal;
    char gain_txt[24];
    char off_txt[24];
    if (argc != 3) {
        SendErr("SYNTAX");
        return;
    }
    if (!ParseU32(argv[2], &channel)) {
        SendErr("SYNTAX");
        return;
    }
    if (channel < 1U || channel > CURRENT_CALC_NUM_CH ||
        CurrentCalc_GetCalibration((uint8_t)channel, &cal) != HAL_OK) {
        SendErr("RANGE");
        return;
    }
    FormatFixed(gain_txt, sizeof(gain_txt), cal.gain, UART_GAIN_DECIMALS);
    FormatFixed(off_txt, sizeof(off_txt), cal.offset_ua, UART_OFFSET_DECIMALS);

    int n = snprintf((char *)s_tx, sizeof(s_tx), "CAL CH=%lu GAIN=%s OFFSET_UA=%s VREF_MV=%u\n",
                     (unsigned long)channel, gain_txt, off_txt,
                     (unsigned)CurrentCalc_GetVrefMillivolts());
    if (n > 0) {
        uint16_t len = (n >= (int)sizeof(s_tx)) ? (uint16_t)(sizeof(s_tx) - 1U)
                                                : (uint16_t)n;
        UartTx(s_tx, len);
    }
    SendOk();
}

static const char *LogStateName(SdLogState_t st)
{
    switch (st) {
        case SD_LOG_ACTIVE:  return "ON";
        case SD_LOG_NO_CARD: return "NOSD";
        case SD_LOG_OFF:
        default:             return "OFF";
    }
}

static void HandleGetCur(int argc)
{
    SdLogStatus_t st;
    char val[20];
    if (argc != 2) {
        SendErr("SYNTAX");
        return;
    }
    SdLogger_GetStatus(&st);

    int n = snprintf((char *)s_tx, sizeof(s_tx), "CUR LOG=%s FILE=%s REC=%lu UA=",
                     LogStateName(st.state), (st.file[0] != '\0') ? st.file : "-",
                     (unsigned long)st.records);
    for (uint8_t i = 0U; i < CURRENT_LOG_NUM_CH && n > 0 && n < (int)sizeof(s_tx); i++) {
        float v = st.last_ua[i];
        if (!st.have_values || v != v) {   /* v != v: NaN */
            (void)snprintf(val, sizeof(val), "NAN");
        } else {
            FormatFixed(val, sizeof(val), v, UART_OFFSET_DECIMALS);
        }
        n += snprintf((char *)s_tx + n, sizeof(s_tx) - (size_t)n, "%s%s",
                      val, (i + 1U < CURRENT_LOG_NUM_CH) ? "," : "\n");
    }
    if (n > 0) {
        uint16_t len = (n >= (int)sizeof(s_tx)) ? (uint16_t)(sizeof(s_tx) - 1U)
                                                : (uint16_t)n;
        UartTx(s_tx, len);
    }
    SendOk();
}

static void HandleEepromSafe(int argc, char **argv)
{
    if (argc != 2 || !StrEqualCI(argv[1], "SAFE")) {
        SendErr("SYNTAX");
        return;
    }
    HAL_StatusTypeDef st = SessionFSM_StoreAmplifierSafeDefault();
    if (st == HAL_BUSY) {
        SendErr("STATE");   /* solo en IDLE */
    } else if (st != HAL_OK) {
        SendErr("HW");
    } else {
        SendOk();
    }
}

static void ProcessLine(void)
{
    char *argv[UART_MAX_TOKENS];
    int argc = Tokenize(s_line, argv, UART_MAX_TOKENS);

    if (argc < 0) {
        SendErr("SYNTAX");   /* demasiados tokens */
        return;
    }
    if (argc == 0) {
        return;              /* linea en blanco: se ignora en silencio */
    }

    if (StrEqualCI(argv[0], "SET")) {
        if (argc < 2) {
            SendErr("SYNTAX");
        } else if (StrEqualCI(argv[1], "AMP")) {
            HandleSetAmp(argc, argv);
        } else if (StrEqualCI(argv[1], "FREQ")) {
            HandleSetFreq(argc, argv);
        } else if (StrEqualCI(argv[1], "WELL")) {
            HandleSetWell(argc, argv);
        } else if (StrEqualCI(argv[1], "CAL")) {
            HandleSetCal(argc, argv);
        } else if (StrEqualCI(argv[1], "VREF")) {
            HandleSetVref(argc, argv);
        } else {
            SendErr("SYNTAX");
        }
    } else if (StrEqualCI(argv[0], "START")) {
        HandleStart(argc, argv);
    } else if (StrEqualCI(argv[0], "STOP")) {
        if (argc != 1) {
            SendErr("SYNTAX");
        } else if (SessionFSM_Stop() != HAL_OK) {
            SendErr("HW");
        } else {
            SendOk();
        }
    } else if (StrEqualCI(argv[0], "STATUS")) {
        if (argc != 1) {
            SendErr("SYNTAX");
        } else {
            SendStatus();
        }
    } else if (StrEqualCI(argv[0], "GET")) {
        if (argc >= 2 && StrEqualCI(argv[1], "CAL")) {
            HandleGetCal(argc, argv);
        } else if (argc >= 2 && StrEqualCI(argv[1], "CUR")) {
            HandleGetCur(argc);
        } else {
            SendErr("SYNTAX");
        }
    } else if (StrEqualCI(argv[0], "SAVE")) {
        if (argc != 2 || !StrEqualCI(argv[1], "CAL")) {
            SendErr("SYNTAX");
        } else if (CurrentCalc_SaveToFlash() != HAL_OK) {
            SendErr("HW");
        } else {
            SendOk();
        }
    } else if (StrEqualCI(argv[0], "EEPROM")) {
        HandleEepromSafe(argc, argv);
    } else {
        SendErr("SYNTAX");
    }
}

/* ------------------------------------------------------------------------- *
 *  API publica
 * ------------------------------------------------------------------------- */
void UartProtocol_Init(void)
{
    s_rx_head = 0U;
    s_rx_tail = 0U;
    s_line_len = 0U;
    s_line_overflow = false;
}

void UartProtocol_RxData(const uint8_t *data, uint32_t len)
{
    if (data == NULL) {
        return;
    }
    for (uint32_t i = 0U; i < len; i++) {
        uint16_t next = (uint16_t)((s_rx_head + 1U) & UART_RX_RING_MASK);
        if (next == s_rx_tail) {
            break;   /* buffer lleno: se descarta el resto */
        }
        s_rx_ring[s_rx_head] = data[i];
        s_rx_head = next;
    }
}

void UartProtocol_Task(void)
{
    while (s_rx_tail != s_rx_head) {
        uint8_t c = s_rx_ring[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) & UART_RX_RING_MASK);

        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            s_line[s_line_len] = '\0';
            if (s_line_overflow) {
                SendErr("SYNTAX");
            } else if (s_line_len > 0U) {
                ProcessLine();
            }
            s_line_len = 0U;
            s_line_overflow = false;
            continue;
        }

        if (s_line_len < UART_LINE_MAX) {
            s_line[s_line_len] = (char)c;
            s_line_len++;
        } else {
            s_line_overflow = true;   /* la linea se descarta hasta el '\n' */
        }
    }
}
