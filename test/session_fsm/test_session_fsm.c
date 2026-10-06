/**
 ******************************************************************************
 * @file    test_session_fsm.c
 * @brief   Arnes de pruebas de escritorio para App/Src/session_fsm.c.
 *
 * Compila la FSM REAL (sin tocarla) contra los mocks de mocks.c /
 * mock_hal.h. Cubre:
 *
 *   1. Calculo del ciclo ON/REST a partir de los parametros de START,
 *      incluido el caso off = 0 (estimulacion continua) y la reanudacion
 *      tras un corte que se salta varios tramos.
 *   2. Rechazo de consignas/parametros fuera de rango (sin aplicar a medias).
 *   3. Recuperacion del backup en RTC_BKP_DR0..DR13:
 *        - backup valido  -> retoma donde estaba;
 *        - CRC corrupto    -> IDLE (0 V);
 *        - magic invalido  -> IDLE;
 *        - parametros imposibles con CRC valido -> IDLE;
 *        - reloj incoherente (marcha atras) -> IDLE.
 *   4. Copia en flash (VBAT unido a VDD: el corte borra el backup Y
 *      reinicia el calendario del RTC):
 *        - punto de control cada 60 s, ademas de en cada transicion/consigna;
 *        - corte total -> reanuda desde el ultimo punto (<= 60 s perdidos,
 *          el tiempo sin alimentacion no cuenta);
 *        - STOP deja un registro IDLE: la sesion parada no resucita;
 *        - si el backup del RTC es valido, manda sobre la flash;
 *        - registro de flash imposible -> IDLE;
 *        - fallo de escritura en flash no detiene la estimulacion.
 *   5. Mantenimiento EEPROM del AD5144 solo en IDLE.
 *   6. Seguridad de salida:
 *        - un pocillo de la mascara con amplitud 0 queda a GND (el AD5144 no
 *          da 0 V por si solo), y se conecta/desconecta al cambiar la
 *          amplitud en caliente;
 *        - un canal cuya escritura en el AD5144 falla queda desconectado;
 *        - START con fallo de hardware -> sesion cancelada y apagado seguro;
 *        - consigna o frecuencia que fallan en caliente -> se conserva el
 *          valor anterior y no se persiste.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "session_fsm.h"
#include "ad5144.h"
#include "dac_waveform.h"
#include "culture_selector.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 *  Mini framework de asserts
 * ------------------------------------------------------------------ */
static int g_checks;
static int g_fail;

#define CHECK(cond, msg) do {                                            \
    g_checks++;                                                          \
    if (!(cond)) { g_fail++; printf("  FAIL (L%d): %s\n", __LINE__, msg); } \
} while (0)

#define CHECK_EQ_U(actual, expect, msg) do {                             \
    g_checks++;                                                          \
    unsigned long _a = (unsigned long)(actual);                          \
    unsigned long _e = (unsigned long)(expect);                          \
    if (_a != _e) { g_fail++;                                            \
        printf("  FAIL (L%d): %s : got %lu, want %lu\n", __LINE__, msg, _a, _e); } \
} while (0)

/* ------------------------------------------------------------------ *
 *  Contexto compartido
 * ------------------------------------------------------------------ */
#define BASE_UNIX      (1700000000ULL)   /* 2023-11-14, lejos de cualquier
                                            frontera de truncado a uint32 */
#define H  (3600UL)
#define D  (86400UL)

static RTC_HandleTypeDef g_hrtc;
static AD5144_Handle_t   g_bankA;
static AD5144_Handle_t   g_bankB;

static void banks_init(void)
{
    AD5144_Init(&g_bankA, NULL, AD5144_I2C_ADDR_ADDR_GND);
    AD5144_Init(&g_bankB, NULL, AD5144_I2C_ADDR_ADDR_VLOG);
}

/* Arranque limpio: reloj a BASE_UNIX, backup y flash borrados, espias a cero. */
static void fresh_boot(void)
{
    MockBKP_Reset();
    MockFlash_Reset();
    MockSpy_Reset();
    MockRTC_SetUnix(BASE_UNIX);
    banks_init();
    CHECK(SessionFSM_Init(&g_hrtc, &g_bankA, &g_bankB) == HAL_OK, "Init HAL_OK");
}

/* Reinicio conservando backup + reloj (simula corte de alimentacion). */
static void reboot_keep_backup(void)
{
    MockSpy_Reset();
    banks_init();
    SessionFSM_Init(&g_hrtc, &g_bankA, &g_bankB);
}

/* CRC-32 identico al de session_fsm.c, para fabricar payloads a medida. */
static uint32_t crc32_ref(const uint32_t *words, uint32_t count)
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

/* Indices del payload de backup (ver session_fsm.c). */
#define W_MAGIC       0
#define W_STATE       1
#define W_SESSION_TS  2
#define W_ON_S        3
#define W_OFF_S       4
#define W_DURATION_S  5
#define W_WORD_COUNT  13
#define W_CRC_REG     13

static void backup_reseal(void)   /* recalcula el CRC tras editar el payload */
{
    uint32_t *b = MockBKP_Raw();
    b[W_CRC_REG] = crc32_ref(b, W_WORD_COUNT);
}

/* ================================================================== *
 *  1. Ciclo ON/REST con off > 0
 * ================================================================== */
static void test_cycle_on_off(void)
{
    printf("[test] ciclo ON/REST (6h/6h, 3 dias)\n");
    fresh_boot();

    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START 6/6/3 aceptado");
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "t=0 -> STIM_ON");

    SessionStatus_t st;

    MockRTC_AdvanceSeconds(1000); SessionFSM_Task();
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.state, SESSION_STATE_STIM_ON, "t=+1000s ON");
    CHECK_EQ_U(st.elapsed_in_state_s, 1000, "ON in_state=1000");
    CHECK_EQ_U(st.elapsed_total_s, 1000, "total=1000");

    MockRTC_AdvanceSeconds(6*H - 1000); SessionFSM_Task();   /* t = 6h exacto */
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.state, SESSION_STATE_STIM_REST, "t=+6h -> REST");
    CHECK_EQ_U(st.elapsed_in_state_s, 0, "REST recien entrado");

    MockRTC_AdvanceSeconds(100); SessionFSM_Task();
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.state, SESSION_STATE_STIM_REST, "REST a +6h100s");
    CHECK_EQ_U(st.elapsed_in_state_s, 100, "REST in_state=100");

    MockRTC_AdvanceSeconds(6*H - 100); SessionFSM_Task();    /* t = 12h */
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "t=+12h -> ON de nuevo");

    /* Corte que se salta varios tramos: de 12h saltamos a 12h + 3 ciclos
     * completos + 8h = 56h  ->  dentro de un tramo REST (8h > 6h de ON) y
     * todavia por debajo de las 72h de duracion total. */
    MockRTC_AdvanceSeconds(3*12*H + 8*H); SessionFSM_Task();
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.state, SESSION_STATE_STIM_REST, "reanuda en REST tras corte largo");
    CHECK_EQ_U(st.elapsed_in_state_s, 2*H, "REST in_state = 8h-6h = 2h");

    /* Fin de sesion: pasado el total, IDLE y backup limpio. */
    MockRTC_SetUnix(BASE_UNIX + 3*D + 10); SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "fin de sesion -> IDLE");
    CHECK_EQ_U(MockBKP_Raw()[W_MAGIC], 0, "backup borrado al terminar");
    CHECK(g_spy.sel_alldisconnect_calls > 0, "pocillos desconectados al terminar");
}

/* ================================================================== *
 *  2. off = 0  ->  estimulacion continua
 * ================================================================== */
static void test_cycle_continuous(void)
{
    printf("[test] estimulacion continua (off=0)\n");
    fresh_boot();

    CHECK(SessionFSM_Start(6*H, 0, 2*D) == HAL_OK, "START 6/0/2 aceptado");
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "t=0 ON");

    MockRTC_AdvanceSeconds(6*H); SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "t=+6h sigue ON (nunca REST)");

    SessionStatus_t st;
    MockRTC_AdvanceSeconds(50000); SessionFSM_Task();
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.state, SESSION_STATE_STIM_ON, "ON a +6h50000s");
    CHECK_EQ_U(st.elapsed_in_state_s, (6*H + 50000) % (6*H), "in_state = total mod on_s");

    MockRTC_SetUnix(BASE_UNIX + 2*D + 1); SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "fin -> IDLE");
}

/* ================================================================== *
 *  3. Rechazo de rango (START y setters), sin tocar el estado previo
 * ================================================================== */
static void test_range_rejection(void)
{
    printf("[test] rechazo de valores fuera de rango\n");
    fresh_boot();

    /* START invalido -> HAL_ERROR y seguimos en IDLE */
    CHECK(SessionFSM_Start(1800, 0, 2*D)      == HAL_ERROR, "on < 1h rechazado");
    CHECK(SessionFSM_Start(6*H, 0, 12*H)      == HAL_ERROR, "duracion < 1 dia rechazada");
    CHECK(SessionFSM_Start(6*H, 200*H, 2*D)   == HAL_ERROR, "off > 168h rechazado");
    CHECK(SessionFSM_Start(6*H, 6*H, 2*D)     == HAL_ERROR ? 0 : 1, "6/6/2 valido (control)");
    /* el control anterior ha armado sesion: la paramos para el resto */
    SessionFSM_Stop();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "STOP -> IDLE");

    /* Setters: fuera de rango -> HAL_ERROR y NO se aplica nada */
    MockSpy_Reset();
    CHECK(SessionFSM_SetChannelMillivolts(3, 2500) == HAL_ERROR, "amp 2500 mV rechazada");
    CHECK(SessionFSM_SetChannelMillivolts(9, 100)  == HAL_ERROR, "canal 9 rechazado");
    CHECK(SessionFSM_SetBankFrequency(DAC_BANK_A, 400000) == HAL_ERROR, "freq 400k rechazada");
    CHECK(SessionFSM_SetBankFrequency(DAC_BANK_A, 0)      == HAL_ERROR, "freq 0 rechazada (< min)");
    CHECK(SessionFSM_SetWell(0, true) == HAL_ERROR, "pocillo 0 rechazado");
    CHECK_EQ_U(g_spy.ad_setmv_calls, 0, "ninguna escritura de amplitud tras rechazos");
    CHECK_EQ_U(g_spy.dac_setfreq_calls, 0, "ninguna escritura de frecuencia tras rechazos");

    /* Consignas validas en IDLE: se guardan pero NO se aplican al hardware */
    CHECK(SessionFSM_SetChannelMillivolts(3, 500) == HAL_OK, "amp 500 mV aceptada");
    CHECK(SessionFSM_SetBankFrequency(DAC_BANK_A, 100000) == HAL_OK, "freq 100k aceptada");
    CHECK_EQ_U(g_spy.ad_setmv_calls, 0, "amp diferida en IDLE (no se aplica)");
    CHECK_EQ_U(g_spy.dac_setfreq_calls, 0, "freq diferida en IDLE (no se aplica)");
}

/* ================================================================== *
 *  4. Consignas vivas: se aplican al entrar en STIM_ON y en caliente
 * ================================================================== */
static void test_live_setpoints(void)
{
    printf("[test] aplicacion de consignas en STIM_ON\n");
    fresh_boot();

    SessionFSM_SetChannelMillivolts(1, 300);
    SessionFSM_SetChannelMillivolts(8, 900);
    SessionFSM_SetBankFrequency(DAC_BANK_A, 100000);
    SessionFSM_SetBankFrequency(DAC_BANK_B, 50000);
    SessionFSM_SetWell(1, true);
    SessionFSM_SetWell(8, true);

    MockSpy_Reset();
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");

    /* Entrada en STIM_ON: vuelca las 8 amplitudes, 2 frecuencias, 8 pocillos */
    CHECK_EQ_U(g_spy.ad_setmv_calls, 8, "ApplyStimOn: 8 amplitudes");
    CHECK_EQ_U(g_spy.dac_setfreq_calls, 2, "ApplyStimOn: 2 frecuencias");
    CHECK_EQ_U(g_spy.sel_setwell_calls, 8, "ApplyStimOn: 8 pocillos");
    CHECK_EQ_U(g_spy.ad_setmv_last_mv[0], 300, "canal 1 -> 300 mV");
    CHECK_EQ_U(g_spy.ad_setmv_last_mv[7], 900, "canal 8 -> 900 mV");
    CHECK_EQ_U(g_spy.dac_setfreq_last[DAC_BANK_A], 100000, "banco A -> 100 kHz");
    CHECK_EQ_U(g_spy.dac_setfreq_last[DAC_BANK_B], 50000,  "banco B -> 50 kHz");
    CHECK_EQ_U(g_spy.sel_well_state, 0x81, "mascara de pocillos 1 y 8");

    /* Cambio en caliente estando en STIM_ON */
    int amp_before = g_spy.ad_setmv_calls;
    CHECK(SessionFSM_SetChannelMillivolts(2, 750) == HAL_OK, "set amp en caliente");
    CHECK_EQ_U(g_spy.ad_setmv_calls, amp_before + 1, "una escritura extra");
    CHECK_EQ_U(g_spy.ad_setmv_last_mv[1], 750, "canal 2 -> 750 mV en caliente");

    /* En STIM_REST las consignas quedan diferidas otra vez */
    MockRTC_AdvanceSeconds(6*H); SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_REST, "en REST");
    int amp_rest = g_spy.ad_setmv_calls;
    CHECK(SessionFSM_SetChannelMillivolts(3, 600) == HAL_OK, "set amp en REST");
    CHECK_EQ_U(g_spy.ad_setmv_calls, amp_rest, "amp diferida en REST");
}

/* ================================================================== *
 *  5. Recuperacion de backup VALIDO
 * ================================================================== */
static void test_recover_valid(void)
{
    printf("[test] recuperacion de backup valido\n");
    fresh_boot();

    SessionFSM_SetChannelMillivolts(4, 1234);
    SessionFSM_SetBankFrequency(DAC_BANK_A, 100000);
    SessionFSM_SetWell(4, true);
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");

    /* +7h: primera vez que toca REST (rem = 7h, on = 6h) */
    MockRTC_AdvanceSeconds(7*H); SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_REST, "pre-corte: REST");

    /* --- corte de alimentacion --- */
    reboot_keep_backup();

    SessionStatus_t st;
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.state, SESSION_STATE_STIM_REST, "post-corte: retoma REST");
    CHECK_EQ_U(st.on_seconds, 6*H, "on_seconds recuperado");
    CHECK_EQ_U(st.off_seconds, 6*H, "off_seconds recuperado");
    CHECK_EQ_U(st.duration_seconds, 3*D, "duration recuperada");
    CHECK_EQ_U(st.amp_mv[3], 1234, "amplitud canal 4 recuperada");
    CHECK_EQ_U(st.freq_hz[DAC_BANK_A], 100000, "frecuencia banco A recuperada");
    CHECK_EQ_U(st.well_mask, 0x08, "mascara de pocillos recuperada");
    CHECK_EQ_U(st.elapsed_total_s, 7*H, "elapsed_total ~ 7h");
    CHECK(g_spy.sel_alldisconnect_calls > 0, "REST fuerza salida segura al reanudar");

    /* Avanza al siguiente tramo ON: debe volcar consignas al hardware */
    MockSpy_Reset();
    MockRTC_AdvanceSeconds(5*H + 100); SessionFSM_Task();   /* t ~ 12h100s -> ON */
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "reanudado -> siguiente ON");
    CHECK_EQ_U(g_spy.ad_setmv_calls, 8, "ON aplica amplitudes tras reanudar");
    CHECK_EQ_U(g_spy.ad_setmv_last_mv[3], 1234, "canal 4 sale a 1234 mV");
    CHECK_EQ_U(g_spy.dac_setfreq_last[DAC_BANK_A], 100000, "banco A a 100 kHz");
}

/* ================================================================== *
 *  6. Backup con CRC corrupto  ->  IDLE
 * ================================================================== */
static void test_recover_bad_crc(void)
{
    printf("[test] backup con CRC corrupto -> IDLE\n");
    fresh_boot();
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");
    MockRTC_AdvanceSeconds(2*H); SessionFSM_Task();

    MockBKP_Raw()[W_DURATION_S] ^= 0x1;   /* payload alterado, CRC sin tocar */
    MockFlash_Reset();   /* se aisla la validacion del backup (sin copia en flash) */

    reboot_keep_backup();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "CRC malo -> IDLE");
    CHECK_EQ_U(MockBKP_Raw()[W_MAGIC], 0, "backup incoherente borrado");
    CHECK(g_spy.sel_alldisconnect_calls > 0, "salida forzada a seguro");
    CHECK(g_spy.ad_allzero_calls > 0, "amplitudes a cero");
}

/* ================================================================== *
 *  7. Backup con magic invalido  ->  IDLE
 * ================================================================== */
static void test_recover_bad_magic(void)
{
    printf("[test] backup con magic invalido -> IDLE\n");
    fresh_boot();
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");
    MockRTC_AdvanceSeconds(2*H); SessionFSM_Task();

    MockBKP_Raw()[W_MAGIC] = 0xDEADBEEFUL;
    MockFlash_Reset();

    reboot_keep_backup();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "magic malo -> IDLE");
}

/* ================================================================== *
 *  8. Parametros imposibles con CRC valido  ->  IDLE
 * ================================================================== */
static void test_recover_implausible_params(void)
{
    printf("[test] parametros imposibles (CRC valido) -> IDLE\n");
    fresh_boot();
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");
    MockRTC_AdvanceSeconds(2*H); SessionFSM_Task();

    /* on_seconds = 0 (por debajo del minimo) pero con CRC recalculado */
    MockBKP_Raw()[W_ON_S] = 0U;
    backup_reseal();
    MockFlash_Reset();

    reboot_keep_backup();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "params fuera de rango -> IDLE");
    CHECK(g_spy.ad_allzero_calls > 0, "salida a 0 V");
}

/* ================================================================== *
 *  9. Reloj incoherente (marcha atras)  ->  IDLE
 * ================================================================== */
static void test_recover_clock_backwards(void)
{
    printf("[test] reloj que retrocede -> IDLE\n");
    fresh_boot();
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");
    MockRTC_AdvanceSeconds(3*H); SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "pre: ON");

    /* El reloj retrocede por debajo del instante de arranque de la sesion. */
    MockRTC_SetUnix(BASE_UNIX - 3600ULL);

    reboot_keep_backup();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "reloj atras -> IDLE");
    CHECK_EQ_U(MockBKP_Raw()[W_MAGIC], 0, "backup borrado");

    /* Y en marcha (sin reinicio), un salto atras tambien cae a IDLE. */
    fresh_boot();
    SessionFSM_Start(6*H, 6*H, 3*D);
    MockRTC_AdvanceSeconds(2*H);
    SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "en marcha: ON");
    MockRTC_SetUnix(BASE_UNIX - 10ULL);
    SessionFSM_Task();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "en marcha, reloj atras -> IDLE");
}

/* ================================================================== *
 *  10. Arranque en frio sin backup  ->  IDLE y 0 V
 * ================================================================== */
static void test_cold_boot_idle(void)
{
    printf("[test] arranque en frio -> IDLE y salida segura\n");
    fresh_boot();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "frio -> IDLE");
    CHECK(g_spy.sel_alldisconnect_calls > 0, "pocillos desconectados");
    CHECK(g_spy.ad_allzero_calls > 0, "amplitudes a 0");

    SessionStatus_t st;
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.elapsed_total_s, 0, "sin sesion: elapsed 0");
    CHECK_EQ_U(st.well_mask, 0, "sin pocillos activos");
}


/* ================================================================== *
 *  11. Copia en flash: puntos de control
 * ================================================================== */
#define Y2000_UNIX   (946684800ULL)   /* 2000-01-01 00:00:00: fecha que fija
                                          MX_RTC_Init tras perder el backup */

/* Indices del payload de flash (ver session_fsm.c). */
#define F_STATE      0
#define F_ELAPSED    1
#define F_ON_S       2
#define F_DURATION   4

/* Corte total: se pierden los registros de backup y el calendario vuelve a
 * 2000-01-01; la flash se conserva. */
static void power_loss_reboot(void)
{
    MockBKP_Reset();
    MockRTC_SetUnix(Y2000_UNIX);
    MockSpy_Reset();
    banks_init();
    SessionFSM_Init(&g_hrtc, &g_bankA, &g_bankB);
}

/* Avanza 'secs' segundos llamando a la FSM una vez por segundo. */
static void run_seconds(uint32_t secs)
{
    for (uint32_t i = 0; i < secs; i++) {
        MockRTC_AdvanceSeconds(1U);
        SessionFSM_Task();
    }
}

static void test_flash_checkpoints(void)
{
    printf("[test] flash: puntos de control\n");
    fresh_boot();
    CHECK_EQ_U(MockFlash_AppendCount(), 0, "arranque en frio no escribe flash");

    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");
    int n0 = MockFlash_AppendCount();
    CHECK(n0 >= 1, "START deja copia en flash");
    CHECK_EQ_U(MockFlash_LastWords()[F_STATE], SESSION_STATE_STIM_ON, "copia: STIM_ON");
    CHECK_EQ_U(MockFlash_LastWords()[F_ELAPSED], 0, "copia: 0 s transcurridos");

    run_seconds(59);
    CHECK_EQ_U(MockFlash_AppendCount(), n0, "59 s: sin punto de control");
    run_seconds(1);
    CHECK_EQ_U(MockFlash_AppendCount(), n0 + 1, "60 s: un punto de control");
    CHECK_EQ_U(MockFlash_LastWords()[F_ELAPSED], 60, "punto de control con 60 s");

    run_seconds(600);
    CHECK_EQ_U(MockFlash_AppendCount(), n0 + 11, "11 minutos: 11 puntos de control");

    int n1 = MockFlash_AppendCount();
    SessionFSM_SetChannelMillivolts(2, 800);
    CHECK_EQ_U(MockFlash_AppendCount(), n1 + 1, "cambio de consigna con sesion armada -> copia");

    /* Un dia entero llamando a la FSM cada segundo: cadencia 1/min */
    fresh_boot();
    SessionFSM_Start(6*H, 6*H, 3*D);
    n0 = MockFlash_AppendCount();
    run_seconds(24U*3600U);
    int per_day = MockFlash_AppendCount() - n0;
    printf("   escrituras en flash en 24 h: %d\n", per_day);
    CHECK(per_day >= 1440 && per_day <= 1444, "1440 puntos/dia (+ transiciones ON/REST)");
}

static void test_flash_power_loss(void)
{
    printf("[test] flash: corte total con perdida del backup y del calendario\n");
    fresh_boot();
    SessionFSM_SetChannelMillivolts(4, 1234);
    SessionFSM_SetBankFrequency(DAC_BANK_A, 100000);
    SessionFSM_SetWell(4, true);
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");

    run_seconds(7U*3600U + 30U);   /* 7 h 30 s: REST, ultimo punto en 7 h 0 min */
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_REST, "pre-corte: REST");

    power_loss_reboot();
    SessionStatus_t st;
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.state, SESSION_STATE_STIM_REST, "post-corte: retoma REST");
    CHECK(st.elapsed_total_s <= 7U*3600U + 30U && st.elapsed_total_s >= 7U*3600U + 30U - 60U,
          "tiempo de sesion = ultimo punto de control (<= 60 s perdidos)");
    CHECK_EQ_U(st.on_seconds, 6*H, "on_seconds recuperado");
    CHECK_EQ_U(st.duration_seconds, 3*D, "duracion recuperada");
    CHECK_EQ_U(st.amp_mv[3], 1234, "amplitud canal 4 recuperada");
    CHECK_EQ_U(st.freq_hz[DAC_BANK_A], 100000, "frecuencia recuperada");
    CHECK_EQ_U(st.well_mask, 0x08, "pocillos recuperados");
    CHECK(MockBKP_Raw()[W_MAGIC] != 0U, "se reconstruye el backup del RTC");

    /* El tiempo sin alimentacion no cuenta: la sesion sigue desde ahi */
    uint32_t t_resume = st.elapsed_total_s;
    MockSpy_Reset();
    run_seconds(12U*3600U - t_resume + 5U);   /* llega a 12 h 5 s de sesion */
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "siguiente ON a las 12 h de sesion");
    CHECK_EQ_U(g_spy.ad_setmv_last_mv[3], 1234, "ON aplica la amplitud recuperada");

    /* Segundo corte en ON, ahora con el backup tambien perdido */
    run_seconds(125);
    power_loss_reboot();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "segundo corte: retoma ON");
    CHECK(g_spy.ad_setmv_calls == 8, "al reanudar en ON se vuelcan las 8 amplitudes");
}

static void test_flash_stop_and_priority(void)
{
    printf("[test] flash: STOP no resucita, backup manda, registro imposible\n");

    /* STOP -> registro IDLE -> tras corte, IDLE */
    fresh_boot();
    SessionFSM_Start(6*H, 6*H, 3*D);
    run_seconds(300);
    SessionFSM_Stop();
    CHECK_EQ_U(MockFlash_LastWords()[F_STATE], SESSION_STATE_IDLE, "STOP deja registro IDLE");
    power_loss_reboot();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "sesion parada no resucita tras corte");
    int n = MockFlash_AppendCount();
    SessionFSM_Stop();
    CHECK_EQ_U(MockFlash_AppendCount(), n, "STOP sin sesion armada no gasta flash");

    /* Backup valido y flash valida: manda el backup (reloj real) */
    fresh_boot();
    SessionFSM_Start(6*H, 6*H, 3*D);
    run_seconds(3600);
    MockFlash_LastWordsRaw()[F_ELAPSED] = 10U;   /* flash "atrasada" a proposito */
    MockRTC_AdvanceSeconds(1800);                 /* corte breve con VBAT */
    reboot_keep_backup();
    SessionStatus_t st;
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.elapsed_total_s, 3600 + 1800, "con backup valido se usa el reloj, no la flash");

    /* Backup corrupto pero flash valida: se reanuda desde la flash */
    fresh_boot();
    SessionFSM_Start(6*H, 6*H, 3*D);
    run_seconds(2U*3600U);
    MockBKP_Raw()[W_DURATION_S] ^= 0x1;
    reboot_keep_backup();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "backup corrupto + flash valida -> ON");

    /* Registro de flash imposible -> IDLE */
    fresh_boot();
    SessionFSM_Start(6*H, 6*H, 3*D);
    run_seconds(120);
    MockFlash_LastWordsRaw()[F_ON_S] = 5U;   /* por debajo del minimo */
    power_loss_reboot();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "flash imposible -> IDLE");
    CHECK(g_spy.ad_allzero_calls > 0, "salida segura");

    /* Tiempo transcurrido >= duracion -> sesion ya terminada -> IDLE */
    fresh_boot();
    SessionFSM_Start(6*H, 6*H, 3*D);
    run_seconds(120);
    MockFlash_LastWordsRaw()[F_ELAPSED] = 3U*D;
    power_loss_reboot();
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "sesion ya cumplida -> IDLE");

    /* Fallo de escritura en flash: la sesion sigue */
    fresh_boot();
    MockFlash_SetWriteFail(true);
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aunque falle la flash");
    run_seconds(180);
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "sigue en ON con flash fallando");
    MockFlash_SetWriteFail(false);
}

static void test_eeprom_maintenance(void)
{
    printf("[test] mantenimiento EEPROM solo en IDLE\n");
    fresh_boot();
    CHECK(SessionFSM_StoreAmplifierSafeDefault() == HAL_OK, "IDLE: permitido");
    CHECK_EQ_U(g_spy.ad_eeprom_calls, 2, "graba los dos AD5144");
    SessionFSM_Start(6*H, 6*H, 3*D);
    CHECK(SessionFSM_StoreAmplifierSafeDefault() == HAL_BUSY, "con sesion: HAL_BUSY");
    CHECK_EQ_U(g_spy.ad_eeprom_calls, 2, "con sesion no toca la EEPROM");
    run_seconds(7U*3600U);
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_REST, "en REST");
    CHECK(SessionFSM_StoreAmplifierSafeDefault() == HAL_BUSY, "en REST tampoco");
}


/* ================================================================== *
 *  12. Seguridad de salida
 * ================================================================== */
static void test_output_safety(void)
{
    printf("[test] seguridad: amplitud 0, fallos de AD5144 y DAC\n");

    /* Amplitud 0 con el pocillo en la mascara -> pocillo a GND */
    fresh_boot();
    SessionFSM_SetBankFrequency(DAC_BANK_A, 100000);
    SessionFSM_SetChannelMillivolts(1, 500);
    SessionFSM_SetWell(1, true);
    SessionFSM_SetWell(2, true);              /* canal 2 sin amplitud */
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START aceptado");
    CHECK_EQ_U(g_spy.sel_well_state & 0x03, 0x01, "pocillo 1 conectado; pocillo 2 (0 mV) a GND");

    SessionFSM_SetChannelMillivolts(2, 300);
    CHECK_EQ_U(g_spy.sel_well_state & 0x02, 0x02, "al darle amplitud, el pocillo 2 se conecta");
    SessionFSM_SetChannelMillivolts(1, 0);
    CHECK_EQ_U(g_spy.sel_well_state & 0x01, 0x00, "al poner 0 mV, el pocillo 1 se desconecta");
    CHECK_EQ_U(g_spy.ad_setmv_last_mv[0], 0, "y el AD5144 pasa a la posicion de 0 mV");
    SessionFSM_SetWell(5, true);              /* canal 5 sin amplitud */
    CHECK_EQ_U(g_spy.sel_well_state & 0x10, 0x00, "SET WELL ON con 0 mV no conecta");
    SessionStatus_t st;
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.well_mask & 0x10, 0x10, "pero la mascara lo recuerda");

    /* Fallo de un AD5144 al entrar en ON: ese pocillo no se conecta */
    fresh_boot();
    for (uint8_t c = 1; c <= 8; c++) {
        SessionFSM_SetChannelMillivolts(c, 400);
        SessionFSM_SetWell(c, true);
    }
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) == HAL_OK, "START con todo bien");
    MockRTC_AdvanceSeconds(7*H); SessionFSM_Task();   /* REST */
    g_spy.ad_fail_mask = 0x04;                         /* canal 3 */
    MockRTC_AdvanceSeconds(6*H); SessionFSM_Task();   /* ON otra vez */
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_STIM_ON, "la sesion sigue en ON");
    CHECK_EQ_U(g_spy.sel_well_state, 0xFB, "todos conectados menos el 3 (escritura fallida)");
    g_spy.ad_fail_mask = 0;

    /* START con fallo de hardware -> cancelada, IDLE, salida segura */
    fresh_boot();
    SessionFSM_SetChannelMillivolts(6, 400);
    SessionFSM_SetWell(6, true);
    g_spy.ad_fail_mask = 0x20;
    CHECK(SessionFSM_StartParamsValid(6*H, 6*H, 3*D), "parametros validos");
    CHECK(SessionFSM_Start(6*H, 6*H, 3*D) != HAL_OK, "START con fallo I2C -> error");
    CHECK_EQ_U(SessionFSM_GetState(), SESSION_STATE_IDLE, "sesion cancelada");
    CHECK_EQ_U(g_spy.sel_well_state, 0x00, "ningun pocillo conectado");
    CHECK_EQ_U(MockBKP_Raw()[W_MAGIC], 0, "sin backup de una sesion que no arranco");
    CHECK_EQ_U(MockFlash_LastWords()[F_STATE], SESSION_STATE_IDLE, "ni copia en flash activa");
    g_spy.ad_fail_mask = 0;
    CHECK(!SessionFSM_StartParamsValid(6*H, 6*H, 3*H), "duracion < tramo ON -> invalido");
    CHECK(!SessionFSM_StartParamsValid(0, 0, 1*D), "ON = 0 -> invalido");

    /* Consigna en caliente que falla -> se conserva la anterior */
    fresh_boot();
    SessionFSM_SetChannelMillivolts(2, 700);
    SessionFSM_SetWell(2, true);
    SessionFSM_Start(6*H, 6*H, 3*D);
    int n = MockFlash_AppendCount();
    g_spy.ad_fail_mask = 0x02;
    CHECK(SessionFSM_SetChannelMillivolts(2, 1500) != HAL_OK, "fallo I2C -> error");
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.amp_mv[1], 700, "se conserva la consigna anterior");
    CHECK_EQ_U(MockFlash_AppendCount(), n, "no se persiste una consigna no aplicada");
    CHECK_EQ_U(g_spy.sel_well_state & 0x02, 0x02, "el pocillo sigue con su amplitud anterior");
    g_spy.ad_fail_mask = 0;

    /* Frecuencia en caliente que falla -> se conserva la anterior */
    SessionFSM_SetBankFrequency(DAC_BANK_B, 50000);
    g_spy.dac_fail = true;
    CHECK(SessionFSM_SetBankFrequency(DAC_BANK_B, 200000) != HAL_OK, "fallo del DAC -> error");
    g_spy.dac_fail = false;
    SessionFSM_GetStatus(&st);
    CHECK_EQ_U(st.freq_hz[DAC_BANK_B], 50000, "se conserva la frecuencia anterior");
    CHECK_EQ_U(g_spy.dac_setfreq_last[DAC_BANK_B], 50000, "y se reaplica en el DAC");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== Arnes de pruebas de session_fsm ===\n");

    test_cold_boot_idle();
    test_cycle_on_off();
    test_cycle_continuous();
    test_range_rejection();
    test_live_setpoints();
    test_recover_valid();
    test_recover_bad_crc();
    test_recover_bad_magic();
    test_recover_implausible_params();
    test_recover_clock_backwards();
    test_flash_checkpoints();
    test_flash_power_loss();
    test_flash_stop_and_priority();
    test_eeprom_maintenance();
    test_output_safety();

    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
