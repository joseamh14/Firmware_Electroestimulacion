/**
 ******************************************************************************
 * @file    test_tia_reader.c
 * @brief   Arnes de pruebas de escritorio para BSP/Src/tia_reader.c y
 *          App/Src/current_log.c.
 *
 * Cubre:
 *   1. Plan de medida segun la frecuencia: banco parado, rafaga de un
 *      periodo (100 kHz, 300 kHz, limite exacto) y rafagas por disparo
 *      (baja f, 4 disparos por periodo del pulso bifasico con reposo).
 *   2. Rotacion 1,2,...,8 con mascara.
 *   3. Modo periodo: un canal por armado, disparo del timer de su banco,
 *      mux cambiado y asentado antes de armar, como mucho un armado por ms,
 *      maximo y minimo acumulados por canal.
 *   4. Banco parado: sus canales se saltan.
 *   5. Modo flancos: cuatro rafagas por periodo, rearme en la propia
 *      interrupcion sin tocar el mux y combinacion de maximo y minimo de
 *      las cuatro (fase positiva, fase negativa y reposo).
 *   6. Rafaga que no llega: se aborta por tiempo y se sigue.
 *   7. Pausa / reanudacion / parada; interrupcion tardia ignorada; fallo
 *      de HAL_ADC_Start_DMA.
 *   8. current_log: amplitud media (I+ - I-)/2, cancelacion del offset,
 *      NaN sin medidas, formato de 36 bytes little-endian.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "tia_reader.h"
#include "current_log.h"
#include "current_calc.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

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

static ADC_HandleTypeDef g_hadc;

static void reset_reader(uint32_t fa, uint32_t fb)
{
    TiaSpy_Reset();
    g_spy.freq[0] = fa;
    g_spy.freq[1] = fb;
    CHECK(TiaReader_Init(&g_hadc) == HAL_OK, "Init -> HAL_OK");
}

/* Un paso del bucle principal: avanza 1 ms y llama a Task. */
static void step_ms(void)
{
    g_spy.tick++;
    TiaReader_Task();
}

/* ================================================================== */
static void test_plan(void)
{
    printf("[test] plan de medida segun la frecuencia\n");
    TiaPlan_t p;
    CHECK_EQ_U(TIA_READER_FS_HZ, 5333333UL, "5,33 Msps con ADC a 80 MHz y 15 ciclos");

    p = TiaReader_PlanFor(0U);
    CHECK(p.mode == TIA_PLAN_SKIP, "0 Hz -> no se mide");

    p = TiaReader_PlanFor(100000U);
    CHECK(p.mode == TIA_PLAN_PERIOD, "100 kHz -> rafaga de un periodo");
    CHECK_EQ_U(p.samples, 54U + 8U, "100 kHz: 54 muestras/periodo + 8 de margen");

    p = TiaReader_PlanFor(300000U);
    CHECK(p.mode == TIA_PLAN_PERIOD, "300 kHz -> periodo");
    CHECK_EQ_U(p.samples, 18U + 8U, "300 kHz: 18 + 8");

    CHECK_EQ_U(TIA_READER_BUF_SAMPLES, 2048U, "buffer de 2048 muestras (4 KB)");
    CHECK_EQ_U(TIA_READER_EDGE_BURSTS, 4U, "4 rafagas por periodo en modo flancos");

    p = TiaReader_PlanFor(2615U);
    CHECK(p.mode == TIA_PLAN_PERIOD && p.samples <= TIA_READER_BUF_SAMPLES, "2615 Hz: aun cabe un periodo");
    CHECK_EQ_U(p.samples, 2048U, "2615 Hz: 2040 muestras/periodo + 8 = buffer completo");
    p = TiaReader_PlanFor(2614U);
    CHECK(p.mode == TIA_PLAN_EDGES, "2614 Hz: ya no cabe -> rafagas por disparo");
    CHECK_EQ_U(p.samples, TIA_READER_EDGE_SAMPLES, "modo flancos: 64 muestras por rafaga");

    p = TiaReader_PlanFor(1U);
    CHECK(p.mode == TIA_PLAN_EDGES, "1 Hz -> rafagas por disparo");

    /* Barrido: una rafaga de periodo siempre cubre >= 1 periodo y cabe */
    int bad = 0;
    for (uint32_t f = 1U; f <= 300000U; f += (f < 10000U) ? 1U : 97U) {
        TiaPlan_t q = TiaReader_PlanFor(f);
        if (q.mode == TIA_PLAN_PERIOD) {
            if ((uint64_t)q.samples * f < TIA_READER_FS_HZ || q.samples > TIA_READER_BUF_SAMPLES) bad++;
        } else if (q.mode == TIA_PLAN_EDGES) {
            /* T/4 (separacion entre disparos) debe ser mayor que la rafaga corta */
            if ((uint64_t)TIA_READER_EDGE_SAMPLES * 4U * f >= TIA_READER_FS_HZ) bad++;
        } else {
            bad++;
        }
    }
    CHECK_EQ_U(bad, 0, "1 Hz..300 kHz: rafaga suficiente y dentro del buffer");
}

static void test_rotation(void)
{
    printf("[test] rotacion de canales\n");
    static const uint8_t expect[] = { 1, 2, 3, 4, 5, 6, 7, 8, 1 };
    uint8_t ch = 0U;
    int bad = 0;
    for (unsigned i = 0; i < sizeof(expect); i++) {
        ch = TiaReader_NextChannel(ch, 0xFFU);
        if (ch != expect[i]) bad++;
    }
    CHECK_EQ_U(bad, 0, "mascara completa: 1,2,3,4,5,6,7,8,1");

    uint8_t m = (uint8_t)((1U << 1) | (1U << 6));   /* canales 2 y 7 */
    CHECK_EQ_U(TiaReader_NextChannel(0, m), 2, "mascara {2,7}: empieza en 2");
    CHECK_EQ_U(TiaReader_NextChannel(2, m), 7, "luego 7");
    CHECK_EQ_U(TiaReader_NextChannel(7, m), 2, "y vuelve a 2");
    CHECK_EQ_U(TiaReader_NextChannel(3, m), 7, "desde un canal fuera de mascara sigue la rotacion");
    CHECK_EQ_U(TiaReader_NextChannel(0, 0), 0, "mascara vacia -> 0");
}

static void test_period_mode(void)
{
    printf("[test] modo periodo (100 kHz en ambos bancos)\n");
    reset_reader(100000U, 100000U);
    CHECK_EQ_U(g_spy.calibs, 1, "calibracion del ADC en Init");
    CHECK(g_spy.mux_disables >= 1, "MAX4581 deshabilitado en Init");

    TiaReader_Start(0xFFU);
    CHECK_EQ_U(g_spy.starts, 0, "Start no arma por si mismo");

    static const int order[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int bad_ch = 0, bad_bank = 0, bad_len = 0;
    for (int r = 0; r < 16; r++) {
        step_ms();
        int a = g_spy.starts - 1;
        if (g_spy.arm_ch[a] != order[r % 8]) bad_ch++;
        if (g_spy.trig_bank[a] != (order[r % 8] <= 4 ? 0 : 1)) bad_bank++;
        if (g_spy.arm_len[a] != 62U) bad_len++;
        /* canal c: maximo 2048+100*c, minimo 2048-100*c */
        int c = order[r % 8];
        TiaSpy_FillBurst((uint16_t)(2048 - 100 * c), (uint16_t)(2048 + 100 * c));
        TiaReader_OnBurstComplete();
        TiaReader_Task();   /* mismo ms: no debe armar otro */
    }
    CHECK_EQ_U(g_spy.starts, 16, "un armado por ms, nunca dos en el mismo ms");
    CHECK_EQ_U(bad_ch, 0, "orden de canales 1,2,3,4,5,6,7,8");
    CHECK_EQ_U(bad_bank, 0, "disparo TIM1 para 1-4 y TIM8 para 5-8");
    CHECK_EQ_U(bad_len, 0, "rafaga de 62 muestras");
    CHECK_EQ_U(g_spy.stops, 16, "ADC parado tras cada rafaga");
    CHECK(g_spy.waits == 16 && g_spy.last_wait_us == TIA_READER_SETTLE_US, "espera de asentamiento tras cada cambio de mux");
    CHECK_EQ_U(g_spy.irq_depth, 0, "secciones criticas equilibradas");

    TiaWindow_t w;
    TiaReader_TakeWindow(&w);
    int bad_acc = 0;
    for (int c = 1; c <= 8; c++) {
        if (w.count[c - 1] != 2) bad_acc++;
        if (w.sum_max[c - 1] != 2U * (2048U + 100U * c)) bad_acc++;
        if (w.sum_min[c - 1] != 2U * (2048U - 100U * c)) bad_acc++;
    }
    CHECK_EQ_U(bad_acc, 0, "2 medidas por canal con su maximo y su minimo");
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0] + w.count[7], 0, "TakeWindow vacia el acumulado");
}

static void test_bank_stopped(void)
{
    printf("[test] banco B parado\n");
    reset_reader(100000U, 0U);
    TiaReader_Start(0xFFU);
    int bad = 0;
    for (int r = 0; r < 8; r++) {
        step_ms();
        int a = g_spy.starts - 1;
        if (g_spy.arm_ch[a] < 1 || g_spy.arm_ch[a] > 4) bad++;
        TiaSpy_FillBurst(2000, 2100);
        TiaReader_OnBurstComplete();
    }
    CHECK_EQ_U(g_spy.starts, 8, "se sigue midiendo el banco A");
    CHECK_EQ_U(bad, 0, "nunca se arma un canal del banco parado");

    reset_reader(0U, 0U);
    TiaReader_Start(0xFFU);
    step_ms(); step_ms();
    CHECK_EQ_U(g_spy.starts, 0, "ambos bancos parados: no se arma nada");
}

static void test_edges(void)
{
    printf("[test] modo flancos (1 kHz, 4 rafagas por periodo)\n");
    reset_reader(1000U, 1000U);
    TiaReader_Start(0x01U);   /* solo canal 1 */
    step_ms();
    CHECK_EQ_U(g_spy.starts, 1, "primera rafaga armada");
    CHECK_EQ_U(g_spy.len, 64U, "64 muestras");
    int sel = g_spy.mux_selects;
    TiaWindow_t w;

    /* Secuencia de un periodo (el primer disparo puede caer en cualquier
     * punto del pulso): reposo, fase positiva, reposo, fase negativa. */
    TiaSpy_FillBurst(2040, 2060);       /* reposo (VBIAS) */
    TiaReader_OnBurstComplete();
    CHECK_EQ_U(g_spy.starts, 2, "la ISR rearma para el disparo siguiente (2/4)");
    TiaSpy_FillBurst(2500, 2600);       /* fase positiva */
    TiaReader_OnBurstComplete();
    CHECK_EQ_U(g_spy.starts, 3, "la ISR rearma para el disparo siguiente (3/4)");
    TiaSpy_FillBurst(2040, 2060);       /* reposo (VBIAS) */
    TiaReader_OnBurstComplete();
    CHECK_EQ_U(g_spy.starts, 4, "la ISR rearma para el disparo siguiente (4/4)");
    CHECK_EQ_U(g_spy.mux_selects, sel, "sin tocar el mux entre las cuatro rafagas");

    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0], 0, "tras tres rafagas aun no hay medida");

    TiaSpy_FillBurst(1500, 1600);       /* fase negativa */
    TiaReader_OnBurstComplete();
    CHECK_EQ_U(g_spy.starts, 4, "tras la cuarta rafaga no se rearma en la ISR");
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0], 1, "tras la cuarta rafaga, una medida");
    CHECK_EQ_U(w.sum_max[0], 2600, "maximo de las cuatro rafagas");
    CHECK_EQ_U(w.sum_min[0], 1500, "minimo de las cuatro rafagas");

    /* Segundo periodo: el acumulado parcial del primero no debe arrastrarse */
    step_ms();
    CHECK_EQ_U(g_spy.starts, 5, "el siguiente periodo lo arma Task");
    for (int k = 0; k < 4; k++) {
        TiaSpy_FillBurst(2040, 2060);
        TiaReader_OnBurstComplete();
    }
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0], 1, "segundo periodo: una medida");
    CHECK_EQ_U(w.sum_max[0], 2060, "sin arrastre del maximo del periodo anterior");
    CHECK_EQ_U(w.sum_min[0], 2040, "sin arrastre del minimo del periodo anterior");

    /* Fallo de rearme a mitad de periodo: la medida se descarta */
    step_ms();
    TiaSpy_FillBurst(2500, 2600);
    TiaReader_OnBurstComplete();                  /* 1/4, rearma */
    g_spy.start_ret = HAL_ERROR;
    TiaSpy_FillBurst(1500, 1600);
    TiaReader_OnBurstComplete();                  /* 2/4, el rearme falla */
    g_spy.start_ret = HAL_OK;
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0], 0, "rearme fallido a mitad de periodo: medida descartada");
    step_ms();                                    /* Task arma un periodo nuevo */
    for (int k = 0; k < 4; k++) {
        TiaSpy_FillBurst(2040, 2060);
        TiaReader_OnBurstComplete();
    }
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0], 1, "tras el fallo, el siguiente periodo se mide entero");
    CHECK_EQ_U(w.sum_max[0], 2060, "sin restos de la medida descartada (maximo)");
    CHECK_EQ_U(w.sum_min[0], 2040, "sin restos de la medida descartada (minimo)");
    CHECK_EQ_U(g_spy.irq_depth, 0, "secciones criticas equilibradas");
}

static void test_timeout(void)
{
    printf("[test] rafaga que no llega\n");
    reset_reader(100000U, 100000U);
    TiaReader_Start(0xFFU);
    step_ms();                        /* arma canal 1; tope = 1 + 20 ms */
    int stops = g_spy.stops;
    for (int i = 0; i < 21; i++) step_ms();
    CHECK_EQ_U(g_spy.stops, stops, "a los 21 ms todavia espera");
    step_ms();
    CHECK_EQ_U(g_spy.stops, stops + 1, "a los 22 ms se aborta");
    step_ms();
    CHECK_EQ_U(g_spy.arm_ch[g_spy.starts - 1], 2, "y se pasa al canal siguiente");
    TiaReader_OnBurstComplete();      /* completa el canal 2 */
    TiaWindow_t w;
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0], 0, "el canal abortado no acumula");
    CHECK_EQ_U(w.count[1], 1, "el siguiente si");
}

static void test_pause_stop(void)
{
    printf("[test] pausa, parada, interrupcion tardia, fallo de Start\n");
    reset_reader(100000U, 100000U);
    TiaReader_Start(0xFFU);
    step_ms();
    int stops = g_spy.stops;
    TiaReader_Pause();
    CHECK_EQ_U(g_spy.stops, stops + 1, "pausa con rafaga pendiente: se aborta");
    TiaSpy_FillBurst(0, 4095);
    TiaReader_OnBurstComplete();      /* interrupcion tardia */
    TiaWindow_t w;
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0], 0, "interrupcion tras abortar: ignorada");
    int starts = g_spy.starts;
    step_ms(); step_ms();
    CHECK_EQ_U(g_spy.starts, starts, "en pausa no se arma");
    TiaReader_Resume();
    step_ms();
    CHECK_EQ_U(g_spy.starts, starts + 1, "tras reanudar se arma");

    TiaReader_Stop();
    CHECK(!TiaReader_IsRunning(), "parado");
    CHECK_EQ_U(g_spy.mux_ch, 0, "MAX4581 deshabilitado al parar");
    starts = g_spy.starts;
    step_ms();
    CHECK_EQ_U(g_spy.starts, starts, "parado no arma");

    reset_reader(100000U, 100000U);
    g_spy.start_ret = HAL_ERROR;
    TiaReader_Start(0xFFU);
    step_ms();
    TiaReader_OnBurstComplete();
    TiaReader_TakeWindow(&w);
    CHECK_EQ_U(w.count[0] + w.count[1], 0, "Start_DMA fallido: nada acumulado");
    g_spy.start_ret = HAL_OK;
    step_ms();
    CHECK(g_spy.starts > 1, "se reintenta en el siguiente ms");
    CHECK_EQ_U(g_spy.irq_depth, 0, "secciones criticas equilibradas");
}

static void test_current_log(void)
{
    printf("[test] current_log: valor por canal y formato\n");
    CurrentCalc_Init();
    TiaWindow_t w;
    memset(&w, 0, sizeof(w));
    /* canal 1: 4 medidas con picos 2048 +/- 1266 -> ~ +/-1000 uA */
    w.count[0] = 4; w.sum_max[0] = 4U * (2048U + 1266U); w.sum_min[0] = 4U * (2048U - 1266U);
    /* canal 2: asimetrico, desplazado +100 codigos: el offset se cancela */
    w.count[1] = 2; w.sum_max[1] = 2U * (2148U + 500U); w.sum_min[1] = 2U * (2148U - 500U);
    float v[8];
    CurrentLog_Compute(&w, v);
    float i1 = 1266.0f * 3300.0f / 4096.0f / 1020.0f * 1000.0f;
    float i2 = 500.0f * 3300.0f / 4096.0f / 1020.0f * 1000.0f;
    CHECK(fabsf(v[0] - i1) < 0.01f, "canal 1: (I+ - I-)/2 = ~1000 uA");
    CHECK(fabsf(v[1] - i2) < 0.01f, "canal 2: el desplazamiento comun no afecta");
    CHECK(v[2] != v[2], "canal sin medidas -> NaN");

    CHECK(CurrentCalc_SetCalibration(1, 1.1f, 25.0f) == HAL_OK, "calibracion canal 1");
    CurrentLog_Compute(&w, v);
    CHECK(fabsf(v[0] - 1.1f * i1) < 0.02f, "ganancia aplicada; el offset de calibracion se cancela");

    /* Inicializador constante (LCC no admite llamadas a funcion en el inicializador). */
    float vals[8] = { 1.0f, -2.5f, 0.0f, 1000.0f, -1000.0f, 0.125f, 3.0f, 0.0f };
    vals[7] = CurrentLog_NaN();
    uint8_t rec[36];
    CurrentLog_Pack(0x01020304UL, vals, rec);
    CHECK(rec[0] == 0x04 && rec[1] == 0x03 && rec[2] == 0x02 && rec[3] == 0x01, "ms en little-endian");
    float back;
    memcpy(&back, &rec[4 + 4 * 1], 4);
    CHECK(back == -2.5f, "float32 del canal 2 intacto");
    CHECK(rec[4] == 0x00 && rec[5] == 0x00 && rec[6] == 0x80 && rec[7] == 0x3F, "1.0f = 00 00 80 3F");
    CHECK(rec[32] == 0x00 && rec[33] == 0x00 && rec[34] == 0xC0 && rec[35] == 0x7F, "NaN = 00 00 C0 7F");
}

int main(void)
{
    printf("=== Arnes de pruebas de tia_reader / current_log ===\n");
    test_plan();
    test_rotation();
    test_period_mode();
    test_bank_stopped();
    test_edges();
    test_timeout();
    test_pause_stop();
    test_current_log();
    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
