/**
 ******************************************************************************
 * @file    test_uart_protocol.c
 * @brief   Arnes de pruebas de escritorio para App/Src/uart_protocol.c.
 *
 * Compila el parser REAL (sin tocarlo) contra:
 *   - un espia de CDC_Transmit_FS (mocks.c) que solo acumula lo transmitido;
 *   - mocks de las cinco funciones mutadoras de session_fsm que invoca el
 *     parser (Start / SetChannelMillivolts / SetBankFrequency / SetWell /
 *     Stop), que registran argumentos sin ejecutar logica de sesion.
 *
 * Se alimenta el parser como en produccion: UartProtocol_RxData() con los
 * bytes de la linea (incluido el '\n') y despues UartProtocol_Task().
 *
 * Cubre:
 *   1. Cada comando documentado dispara la llamada correcta con los
 *      argumentos correctos (y las horas de START pasan a segundos).
 *   2. Token no numerico / desbordamiento de entero / nº de argumentos
 *      incorrecto -> ERR SYNTAX, sin llamar a session_fsm.
 *   3. Valor numericamente valido pero fuera de rango -> ERR RANGE, sin
 *      llamar a session_fsm.
 *   4. Las palabras clave no distinguen mayusculas/minusculas.
 *   5. Una linea mas larga que UART_LINE_MAX no desborda el buffer ni
 *      corrompe el siguiente comando.
 *   6. Un fallo simulado de session_fsm (mock -> HAL_ERROR) se traduce en
 *      ERR HW (ERR RANGE en el caso de START, por diseno) y nunca en OK.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "usbd_cdc_if.h"
#include "fsm_spy.h"

#include "uart_protocol.h"
#include "session_fsm.h"
#include "dac_waveform.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 *  Mini framework de asserts (mismo estilo que test/session_fsm/)
 * ------------------------------------------------------------------ */
static int g_checks;
static int g_fail;

#define CHECK(cond, msg) do {                                            \
    g_checks++;                                                          \
    if (!(cond)) { g_fail++; printf("  FALLO (L%d): %s\n", __LINE__, msg); } \
} while (0)

#define CHECK_EQ_U(actual, expect, msg) do {                             \
    g_checks++;                                                          \
    unsigned long _a = (unsigned long)(actual);                          \
    unsigned long _e = (unsigned long)(expect);                          \
    if (_a != _e) { g_fail++;                                            \
        printf("  FALLO (L%d): %s : got %lu, want %lu\n", __LINE__, msg, _a, _e); } \
} while (0)

#define CHECK_TX(expect, msg) do {                                       \
    g_checks++;                                                          \
    if (strcmp(g_tx.buf, (expect)) != 0) { g_fail++;                     \
        printf("  FALLO (L%d): %s : tx=\"%s\" want \"%s\"\n",             \
               __LINE__, msg, g_tx.buf, (expect)); }                     \
} while (0)

/* ------------------------------------------------------------------ *
 *  Utilidades
 * ------------------------------------------------------------------ */
static void reset_all(void)
{
    FsmSpy_Reset();
    TxSpy_Reset();
    MockUsb_SetConfigured(true);
    UartProtocol_Init();
}

/* Entrega una cadena al parser (incluye su '\n') y procesa. */
static void feed(const char *s)
{
    UartProtocol_RxData((const uint8_t *)s, (uint32_t)strlen(s));
    UartProtocol_Task();
}

static bool tx_has(const char *needle) { return strstr(g_tx.buf, needle) != NULL; }

static bool tx_ends_with(const char *suffix)
{
    size_t n = strlen(g_tx.buf);
    size_t m = strlen(suffix);
    return (n >= m) && (strcmp(g_tx.buf + n - m, suffix) == 0);
}

/* ================================================================== *
 *  1. Cada comando dispara la llamada correcta
 * ================================================================== */
static void test_commands_dispatch(void)
{
    printf("[test] cada comando -> llamada correcta con argumentos correctos\n");

    reset_all();
    feed("SET AMP 3 500\n");
    CHECK_EQ_U(g_fsm.setamp_calls, 1,   "SET AMP -> 1 llamada");
    CHECK_EQ_U(g_fsm.setamp_channel, 3, "SET AMP -> canal 3");
    CHECK_EQ_U(g_fsm.setamp_mv, 500,    "SET AMP -> 500 mV");
    CHECK_TX("OK\n", "SET AMP -> OK");

    reset_all();
    feed("SET FREQ A 100000\n");
    CHECK_EQ_U(g_fsm.setfreq_calls, 1,            "SET FREQ -> 1 llamada");
    CHECK_EQ_U(g_fsm.setfreq_bank, DAC_BANK_A,    "SET FREQ -> banco A");
    CHECK_EQ_U(g_fsm.setfreq_hz, 100000,          "SET FREQ -> 100000 Hz");
    CHECK_TX("OK\n", "SET FREQ -> OK");

    reset_all();
    feed("SET FREQ B 1\n");
    CHECK_EQ_U(g_fsm.setfreq_bank, DAC_BANK_B, "SET FREQ B -> banco B");
    CHECK_EQ_U(g_fsm.setfreq_hz, 1,            "SET FREQ B 1 -> 1 Hz (limite inferior)");

    reset_all();
    feed("SET WELL 8 ON\n");
    CHECK_EQ_U(g_fsm.setwell_calls, 1,     "SET WELL -> 1 llamada");
    CHECK_EQ_U(g_fsm.setwell_channel, 8,   "SET WELL -> canal 8");
    CHECK_EQ_U(g_fsm.setwell_connected, 1, "SET WELL ON -> connected=true");
    CHECK_TX("OK\n", "SET WELL -> OK");

    reset_all();
    feed("SET WELL 2 OFF\n");
    CHECK_EQ_U(g_fsm.setwell_channel, 2,   "SET WELL 2 -> canal 2");
    CHECK_EQ_U(g_fsm.setwell_connected, 0, "SET WELL OFF -> connected=false");

    reset_all();
    feed("START 6 6 3\n");
    CHECK_EQ_U(g_fsm.start_calls, 1,               "START -> 1 llamada");
    CHECK_EQ_U(g_fsm.start_on_s, 6UL * 3600UL,     "START horas_on -> segundos");
    CHECK_EQ_U(g_fsm.start_off_s, 6UL * 3600UL,    "START horas_off -> segundos");
    CHECK_EQ_U(g_fsm.start_duration_s, 3UL * 86400UL, "START dias -> segundos");
    CHECK_TX("OK\n", "START -> OK");

    reset_all();
    feed("STOP\n");
    CHECK_EQ_U(g_fsm.stop_calls, 1, "STOP -> 1 llamada");
    CHECK_TX("OK\n", "STOP -> OK");

    reset_all();
    feed("STATUS\n");
    CHECK(g_fsm.getstatus_calls >= 1, "STATUS -> consulta GetStatus");
    CHECK_EQ_U(g_fsm.mutations, 0,     "STATUS no muta la FSM");
    CHECK(tx_has("STATE=IDLE"),        "STATUS incluye STATE=");
    CHECK(tx_ends_with("OK\n"),        "STATUS termina en OK");
}

/* ================================================================== *
 *  2. ERR SYNTAX: token no numerico / overflow / argc, sin tocar la FSM
 * ================================================================== */
static void test_syntax_errors(void)
{
    printf("[test] ERR SYNTAX (token invalido / overflow / argc) sin llamar a la FSM\n");

    const char *bad[] = {
        "SET AMP x 500\n",          /* token no numerico             */
        "SET AMP 3 seiscientos\n",  /* token no numerico             */
        "SET AMP 3 4294967296\n",   /* 2^32: desbordamiento de uint32 */
        "SET FREQ A 99999999999\n", /* desbordamiento de uint32      */
        "SET AMP 3\n",              /* argc corto                    */
        "SET AMP 3 500 7\n",        /* argc largo                    */
        "SET AMP 3 500 7 8\n",      /* > UART_MAX_TOKENS              */
        "START 6 6\n",              /* argc corto                    */
        "START 6 6 3 9\n",          /* argc largo                    */
        "STOP ahora\n",             /* STOP no admite argumentos     */
        "STATUS ya\n",              /* STATUS no admite argumentos   */
        "FOOBAR\n",                 /* comando desconocido           */
        "SET LADO 1 2\n",           /* subcomando SET desconocido    */
        "SET FREQ C 100\n",         /* banco invalido (no A/B)       */
        "SET WELL 3 QUIZAS\n",      /* ni ON ni OFF                   */
    };

    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset_all();
        feed(bad[i]);
        CHECK_TX("ERR SYNTAX\n", bad[i]);
        CHECK_EQ_U(g_fsm.mutations, 0, "SYNTAX -> sin llamadas a la FSM");
    }
}

/* ================================================================== *
 *  3. ERR RANGE: numericamente valido pero fuera de rango
 * ================================================================== */
static void test_range_errors(void)
{
    printf("[test] ERR RANGE (valor valido pero fuera de rango) sin llamar a la FSM\n");

    const char *bad[] = {
        "SET AMP 3 2001\n",    /* > SESSION_AMP_MAX_MV (2000)         */
        "SET AMP 3 65535\n",   /* muy por encima                     */
        "SET AMP 0 100\n",     /* canal < 1                          */
        "SET AMP 9 100\n",     /* canal > 8                          */
        "SET FREQ A 0\n",      /* < DAC_FREQ_MIN_HZ (1)              */
        "SET FREQ A 300001\n", /* > DAC_FREQ_MAX_HZ (300000)         */
        "SET FREQ B 400000\n", /* > 300000                           */
        "SET WELL 0 ON\n",     /* pocillo < 1                        */
        "SET WELL 9 ON\n",     /* pocillo > 8                        */
        "START 0 6 3\n",       /* horas_on < 1                       */
        "START 6 6 0\n",       /* dias < 1                           */
        "START 200 6 3\n",     /* horas_on > 168                     */
        "START 6 200 3\n",     /* horas_off > 168                    */
        "START 6 6 400\n",     /* dias > 365                         */
    };

    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset_all();
        feed(bad[i]);
        CHECK_TX("ERR RANGE\n", bad[i]);
        CHECK_EQ_U(g_fsm.mutations, 0, "RANGE -> sin llamadas a la FSM");
    }
}

/* ================================================================== *
 *  4. Palabras clave sin distinguir mayusculas/minusculas
 * ================================================================== */
static void test_case_insensitive(void)
{
    printf("[test] palabras clave case-insensitive\n");

    reset_all();
    feed("set amp 4 250\n");
    CHECK_EQ_U(g_fsm.setamp_calls, 1,   "'set amp' minusculas reconocido");
    CHECK_EQ_U(g_fsm.setamp_channel, 4, "canal 4");
    CHECK_EQ_U(g_fsm.setamp_mv, 250,    "250 mV");
    CHECK_TX("OK\n", "set amp -> OK");

    reset_all();
    feed("SeT fReQ a 50000\n");
    CHECK_EQ_U(g_fsm.setfreq_calls, 1,         "'SeT fReQ' mixto reconocido");
    CHECK_EQ_U(g_fsm.setfreq_bank, DAC_BANK_A, "banco 'a' minuscula -> A");

    reset_all();
    feed("set well 5 oN\n");
    CHECK_EQ_U(g_fsm.setwell_connected, 1, "'oN' -> true");

    reset_all();
    feed("StArT 6 6 3\n");
    CHECK_EQ_U(g_fsm.start_calls, 1, "'StArT' reconocido");

    reset_all();
    feed("status\n");
    CHECK(tx_has("STATE="), "'status' minuscula reconocido");

    reset_all();
    feed("stop\n");
    CHECK_EQ_U(g_fsm.stop_calls, 1, "'stop' minuscula reconocido");
}

/* ================================================================== *
 *  5. Linea > UART_LINE_MAX: sin desbordar, sin corromper el siguiente
 * ================================================================== */
static void test_line_overflow(void)
{
    printf("[test] linea sobredimensionada: sin desbordar ni corromper el siguiente comando\n");

    /* Linea de ~200 'A' sin sentido, terminada en '\n' -> muy por encima de
     * UART_LINE_MAX. Debe responder ERR SYNTAX y NO llamar a la FSM. */
    reset_all();
    {
        char big[220];
        memset(big, 'A', 200);
        big[200] = '\n';
        big[201] = '\0';
        feed(big);
    }
    CHECK_TX("ERR SYNTAX\n", "linea larga -> ERR SYNTAX");
    CHECK_EQ_U(g_fsm.mutations, 0, "linea larga no llama a la FSM");

    /* El siguiente comando se procesa limpio (buffer de linea reseteado). */
    TxSpy_Reset();
    feed("SET AMP 5 750\n");
    CHECK_EQ_U(g_fsm.setamp_calls, 1,   "comando tras overflow: parsea");
    CHECK_EQ_U(g_fsm.setamp_channel, 5, "canal correcto tras overflow");
    CHECK_EQ_U(g_fsm.setamp_mv, 750,    "mV correcto tras overflow");
    CHECK_TX("OK\n", "OK tras overflow");

    /* Overflow entregado en dos trozos, a caballo de dos UartProtocol_Task. */
    reset_all();
    {
        char part[130];
        memset(part, 'B', 120);
        part[120] = '\0';
        feed(part);                       /* 120 bytes, aun sin '\n' */
    }
    CHECK_EQ_U(g_tx.calls, 0, "sin respuesta hasta el '\\n'");
    feed("resto\n");
    CHECK_TX("ERR SYNTAX\n", "overflow troceado -> ERR SYNTAX");
    TxSpy_Reset();
    feed("STATUS\n");
    CHECK(tx_has("STATE="), "STATUS tras overflow troceado: OK");

    /* Linea de exactamente UART_LINE_MAX (64) caracteres: NO desborda. */
    reset_all();
    {
        char exact[80];
        int k;
        strcpy(exact, "SET AMP 3 500");     /* 13 caracteres */
        for (k = 13; k < 64; k++) { exact[k] = ' '; }
        exact[64] = '\n';
        exact[65] = '\0';
        feed(exact);
    }
    CHECK_EQ_U(g_fsm.setamp_calls, 1, "linea de 64 exactos: parsea");
    CHECK_TX("OK\n", "linea de 64 exactos -> OK");

    /* Un caracter mas (65) ya desborda -> ERR SYNTAX, sin llegar a la FSM. */
    reset_all();
    {
        char over[80];
        int k;
        strcpy(over, "SET AMP 3 500");
        for (k = 13; k < 65; k++) { over[k] = ' '; }
        over[65] = '\n';
        over[66] = '\0';
        feed(over);
    }
    CHECK_TX("ERR SYNTAX\n", "linea de 65 -> overflow -> ERR SYNTAX");
    CHECK_EQ_U(g_fsm.mutations, 0, "linea de 65 no llega a la FSM");
}

/* ================================================================== *
 *  6. Fallo simulado de session_fsm -> ERR HW / ERR RANGE, nunca OK
 * ================================================================== */
static void test_fsm_failure(void)
{
    printf("[test] fallo simulado de session_fsm -> ERR HW (RANGE en START), nunca OK\n");

    reset_all(); g_fsm.ret = HAL_ERROR;
    feed("SET AMP 3 500\n");
    CHECK_TX("ERR HW\n", "fallo FSM en SET AMP -> ERR HW");
    CHECK_EQ_U(g_fsm.setamp_calls, 1, "la FSM SI se invoco (el valor era valido)");

    reset_all(); g_fsm.ret = HAL_ERROR;
    feed("SET FREQ A 100000\n");
    CHECK_TX("ERR HW\n", "fallo FSM en SET FREQ -> ERR HW");

    reset_all(); g_fsm.ret = HAL_ERROR;
    feed("SET WELL 3 ON\n");
    CHECK_TX("ERR HW\n", "fallo FSM en SET WELL -> ERR HW");

    reset_all(); g_fsm.ret = HAL_ERROR;
    feed("STOP\n");
    CHECK_TX("ERR HW\n", "fallo FSM en STOP -> ERR HW");

    /* HandleStart mapea un fallo tardio de SessionFSM_Start a ERR RANGE
     * (la combinacion de tiempos puede ser incoherente); ver el comentario
     * en uart_protocol.c. Lo importante: NO devuelve OK. */
    reset_all(); g_fsm.ret = HAL_ERROR;
    feed("START 6 6 3\n");
    CHECK_TX("ERR RANGE\n", "fallo de SessionFSM_Start -> ERR RANGE (no OK)");
    CHECK_EQ_U(g_fsm.start_calls, 1, "START si invoco a la FSM");
}

/* ================================================================== *
 *  Extra: parser independiente de la pila USB
 * ================================================================== */
static void test_usb_not_ready(void)
{
    printf("[test] USB no configurado: el comando se procesa, la respuesta se descarta\n");

    reset_all();
    MockUsb_SetConfigured(false);
    feed("SET AMP 3 500\n");
    CHECK_EQ_U(g_fsm.setamp_calls, 1, "comando procesado aunque USB no este listo");
    CHECK_EQ_U(g_tx.calls, 0,         "respuesta descartada si pClassData == NULL");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== Arnes de pruebas de uart_protocol ===\n");

    test_commands_dispatch();
    test_syntax_errors();
    test_range_errors();
    test_case_insensitive();
    test_line_overflow();
    test_fsm_failure();
    test_usb_not_ready();

    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
