/**
 ******************************************************************************
 * @file    test_dac_waveform.c
 * @brief   Arnes de pruebas de escritorio para el calculo de PSC/ARR de
 *          BSP/Src/dac_waveform.c.
 *
 * La funcion ComputeTimerParams() es aritmetica pura: a partir de una
 * frecuencia de DISPARO (4x la de la onda, por las 4 muestras/periodo del
 * buffer DMA: pulso bifasico con reposo HIGH, LOW, MID, MID) busca el par
 * (prescaler, autorecarga) que mejor la aproxima, con ARR+1 redondeado al
 * entero mas proximo, sin que ARR+1 se salga de 16 bits. No toca ningun
 * registro, asi que se prueba sin mocks de comportamiento.
 *
 * Como ComputeTimerParams es 'static', el TU de test incluye el .c entero
 * (tecnica habitual para test de funciones internas). Los stubs no-op de
 * mocks.c solo sirven para que DacWaveform_Init / _SetFrequency / _Stop
 * enlacen y se puedan ejercitar tambien por el API publico, leyendo el
 * PSC/ARR resultante en los "registros" del handle de timer simulado.
 *
 * Toda la aritmetica de verificacion es entera de 64 bits (sin coma
 * flotante): el error relativo se expresa en ppm (partes por millon).
 *
 * Cubre:
 *   1. Tres puntos de referencia (con 4 disparos por periodo):
 *      1 Hz -> PSC=457/ARR=65501, 100 kHz -> 0/299, 300 kHz -> 0/99.
 *   2. Limite inferior (1 Hz) y rechazo de 0 Hz (no se interpreta como
 *      "sin division"): salida sin tocar, retorno false.
 *   3. Techo aritmetico exacto sin prescaler (trigger = Fclk -> ARR=0). El
 *      limite superior UTIL (300 kHz) se prueba por el API publico (punto 6).
 *   4. Barrido de toda la banda 1 Hz - 300 kHz: nunca rechaza, ARR/PSC
 *      siempre en 16 bits (no hay desbordamiento silencioso del registro),
 *      error relativo acotado.
 *   5. Barrido por decadas (10/100/1k/10k Hz): error relativo de pocos ppm.
 *   6. Via API publico DacWaveform_SetFrequency: el par (PSC, ARR) llega al
 *      timer, el limite superior se rechaza sin tocar el estado anterior,
 *      0 Hz para el banco y un fallo de DMA se propaga.
 *   7. Pulso bifasico con reposo centrado en VBIAS y aparcado (revision de
 *      esquematicos de octubre de 2026): buffer de 4 muestras HIGH, LOW, MID,
 *      MID (T/4, T/4, T/2), codigos 2048 +/- 621, amplitud de Dif de 500 mV;
 *      Init, Stop, 0 Hz y un fallo de DMA dejan el canal HABILITADO en 2048
 *      con el disparo por TRGO desactivado (nunca flotante), y al arrancar
 *      la onda el disparo vuelve a estar activo.
 ******************************************************************************
 */
#include "mock_hal.h"

#include <stdio.h>

/* La funcion bajo prueba y todo dac_waveform.c, sin modificar. */
#include "dac_waveform.c"

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

/* Divisor total del timer para un par (psc, arr). */
static uint64_t total_div(uint32_t psc, uint32_t arr)
{
    return (uint64_t)(psc + 1U) * (uint64_t)(arr + 1U);
}

/* Error relativo (ppm) entre la ONDA que produce (psc, arr) y la frecuencia
 * de onda pedida, con aritmetica entera:
 *   err = |Fclk - N*f*D| / (N*f*D),   D = (psc+1)*(arr+1),   N = 4
 * (el timer dispara a Fclk/D y la onda sale a la cuarta parte). */
static uint64_t rel_err_ppm(uint32_t wave_hz, uint32_t psc, uint32_t arr)
{
    uint64_t den  = (uint64_t)DAC_WAVE_SAMPLES * (uint64_t)wave_hz * total_div(psc, arr);
    uint64_t fclk = (uint64_t)DAC_TRIGGER_CLOCK_HZ;
    uint64_t num  = (fclk > den) ? (fclk - den) : (den - fclk);
    return (num * 1000000ULL) / den;
}

/* Frecuencia de onda reconstruida, en milihercios, para los volcados. */
static uint64_t wave_mhz(uint32_t psc, uint32_t arr)
{
    return ((uint64_t)DAC_TRIGGER_CLOCK_HZ * 1000ULL) / ((uint64_t)DAC_WAVE_SAMPLES * total_div(psc, arr));
}

/* ComputeTimerParams trabaja sobre la frecuencia de DISPARO; este wrapper
 * lo llama en terminos de la frecuencia de ONDA (la que ve el usuario). */
static bool params_for_wave(uint32_t wave_hz, uint32_t *psc, uint32_t *arr)
{
    return ComputeTimerParams(wave_hz * DAC_WAVE_SAMPLES, psc, arr);
}

/* ================================================================== *
 *  1. Puntos de referencia
 * ================================================================== */
static void test_reference_points(void)
{
    printf("[test] puntos de referencia (4 disparos por periodo)\n");
    uint32_t psc, arr;

    CHECK_EQ_U(DAC_WAVE_SAMPLES, 4U, "4 muestras por periodo (HIGH, LOW, MID, MID)");

    CHECK(params_for_wave(1U, &psc, &arr), "1 Hz: calculable");
    CHECK_EQ_U(psc, 457U,   "1 Hz -> PSC=457");
    CHECK_EQ_U(arr, 65501U, "1 Hz -> ARR=65501");

    CHECK(params_for_wave(100000U, &psc, &arr), "100 kHz: calculable");
    CHECK_EQ_U(psc, 0U,   "100 kHz -> PSC=0");
    CHECK_EQ_U(arr, 299U, "100 kHz -> ARR=299 (fase de 2,5 us)");

    CHECK(params_for_wave(300000U, &psc, &arr), "300 kHz: calculable");
    CHECK_EQ_U(psc, 0U,   "300 kHz -> PSC=0");
    CHECK_EQ_U(arr, 99U,  "300 kHz -> ARR=99 (fase de 0,83 us)");
}

/* ================================================================== *
 *  2 y 3. Limite inferior, 0 Hz y techo aritmetico
 * ================================================================== */
static void test_limits_and_zero(void)
{
    printf("[test] limite inferior, entrada 0 Hz y techo aritmetico\n");
    uint32_t psc = 0xDEADU;
    uint32_t arr = 0xBEEFU;

    /* 0 Hz: rechazo limpio, ni "sin division" ni parametros a medias. */
    CHECK(!ComputeTimerParams(0U, &psc, &arr), "trigger 0 Hz -> false");
    CHECK(!params_for_wave(0U, &psc, &arr),    "onda 0 Hz -> false");
    CHECK_EQ_U(psc, 0xDEADU, "0 Hz no escribe *out_psc");
    CHECK_EQ_U(arr, 0xBEEFU, "0 Hz no escribe *out_arr");

    /* Limite inferior de la banda soportada. */
    CHECK(params_for_wave(DAC_FREQ_MIN_HZ, &psc, &arr), "1 Hz (limite inferior): calculable");
    CHECK(arr <= 0xFFFFU, "1 Hz: ARR cabe en 16 bits");
    CHECK(psc <= 0xFFFFU, "1 Hz: PSC cabe en 16 bits");

    /* Techo aritmetico con PSC=0: ARR+1 = Fclk/trigger. En trigger = Fclk da
     * ARR+1 = 1 (ARR=0), la maxima frecuencia representable sin prescaler. */
    CHECK(ComputeTimerParams(DAC_TRIGGER_CLOCK_HZ, &psc, &arr), "trigger = Fclk: calculable");
    CHECK_EQ_U(psc, 0U, "trigger = Fclk -> PSC=0");
    CHECK_EQ_U(arr, 0U, "trigger = Fclk -> ARR=0");

    /* El limite superior UTIL de frecuencia lo fija DAC_FREQ_MAX_HZ (300 kHz)
     * y lo hace cumplir DacWaveform_SetFrequency antes de llamar aqui -- se
     * prueba por el API publico en test_public_api (300000 pasa, 300001 ->
     * HAL_ERROR). Que el ARR entregado quepa SIEMPRE en el registro de 16
     * bits en todo el rango soportado se comprueba en test_band_sweep.
     *
     * NOTA: por encima de ~119 MHz de disparo el producto trigger*(psc+1) se
     * sale de uint32 y el buscador puede devolver un par impreciso; ese
     * camino es inalcanzable desde el API publico (filtrado a 300 kHz) y no
     * llega a desbordar el registro de 16 bits, pero queda anotado. */
}

/* ================================================================== *
 *  4. Banda completa: sin rechazos, sin overflow de 16 bits
 * ================================================================== */
static void test_band_sweep(void)
{
    printf("[test] barrido 1 Hz - 300 kHz: sin rechazos ni overflow de 16 bits\n");

    int rejects = 0;
    int overflows = 0;
    uint64_t worst_ppm = 0U;
    uint32_t worst_f = 0U;

    for (uint32_t f = DAC_FREQ_MIN_HZ; f <= DAC_FREQ_MAX_HZ; f++) {
        uint32_t psc, arr;
        if (!ComputeTimerParams(f * DAC_WAVE_SAMPLES, &psc, &arr)) {
            rejects++;
            continue;
        }
        if (arr > 0xFFFFU || psc > 0xFFFFU) {
            overflows++;
            continue;
        }
        uint64_t e = rel_err_ppm(f, psc, arr);
        if (e > worst_ppm) { worst_ppm = e; worst_f = f; }
    }

    CHECK_EQ_U(rejects, 0,   "ningun rechazo dentro de [1, 300000] Hz");
    CHECK_EQ_U(overflows, 0, "ARR y PSC siempre dentro de 16 bits en toda la banda");
    /* Con ARR+1 redondeado, el error maximo es medio paso del contador:
     * 0,5/(ARR+1), que a 300 kHz (ARR+1 = 100) llega a 5000 ppm = 0,5 %. Es
     * el limite del requisito de error de frecuencia < 1 % (tabla 8.2 de la
     * memoria de la etapa analogica). El barrido solo garantiza que el
     * buscador converge y acota el error, no la exactitud fina -- esa se
     * mide en los puntos de decada. */
    CHECK(worst_ppm <= 5000U, "error relativo de banda <= 5000 ppm (0,5 %)");
    printf("   peor error relativo de banda: %llu ppm en f = %lu Hz\n",
           (unsigned long long)worst_ppm, (unsigned long)worst_f);
}

/* ================================================================== *
 *  5. Barrido por decadas: error dentro del orden del Anexo
 * ================================================================== */
static void test_decade_error(void)
{
    printf("[test] barrido por decadas: error relativo dentro del margen del Anexo\n");

    static const uint32_t freqs[] = { 10U, 100U, 1000U, 10000U };

    /* 100 Hz, 1 kHz y 10 kHz dividen exacto; 10 Hz tiene un error real de
     * ~6 ppm (65217,39 redondeado a 65217). Todos por debajo de 10 ppm
     * (1e-3 %%). */
    const uint64_t tol_ppm = 10U;

    for (size_t i = 0U; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        uint32_t f = freqs[i];
        uint32_t psc, arr;
        CHECK(params_for_wave(f, &psc, &arr), "decada: calculable");

        uint64_t e = rel_err_ppm(f, psc, arr);
        uint64_t mhz = wave_mhz(psc, arr);

        char msg[96];
        snprintf(msg, sizeof(msg), "%lu Hz: error %llu ppm <= %llu ppm",
                 (unsigned long)f, (unsigned long long)e, (unsigned long long)tol_ppm);
        CHECK(e <= tol_ppm, msg);

        printf("   %6lu Hz -> PSC=%lu ARR=%lu  f_real=%llu.%03llu Hz  err=%llu ppm\n",
               (unsigned long)f, (unsigned long)psc, (unsigned long)arr,
               (unsigned long long)(mhz / 1000ULL), (unsigned long long)(mhz % 1000ULL),
               (unsigned long long)e);
    }
}

/* ================================================================== *
 *  6. Via API publico: PSC/ARR reales en el "registro" del timer
 * ================================================================== */
static DAC_TypeDef       s_dac_regs;
static DAC_HandleTypeDef s_hdac_stub = { &s_dac_regs };
static TIM_HandleTypeDef s_htimA;
static TIM_HandleTypeDef s_htimB;

static void dac_setup(void)
{
    DacHalSpy_Reset();
    s_dac_regs.CR = DAC_CR_TEN1 | (DAC_CR_TEN1 << 16);   /* como tras MX_DAC1_Init */
    s_htimA.PSC = s_htimA.ARR = s_htimA.CNT = 0xFFFFFFFFU;
    s_htimB.PSC = s_htimB.ARR = s_htimB.CNT = 0xFFFFFFFFU;
    CHECK(DacWaveform_Init(&s_hdac_stub, &s_htimA, &s_htimB) == HAL_OK, "DacWaveform_Init -> HAL_OK");
}

static void test_public_api(void)
{
    printf("[test] DacWaveform_SetFrequency: (PSC, ARR) via registros del timer\n");
    dac_setup();

    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 100000U) == HAL_OK, "SET 100 kHz -> HAL_OK");
    CHECK_EQ_U(s_htimA.PSC, 0U,   "100 kHz -> TIMx->PSC");
    CHECK_EQ_U(s_htimA.ARR, 299U, "100 kHz -> TIMx->ARR");
    CHECK_EQ_U(s_htimA.CNT, 0U,   "reconfig pone TIMx->CNT a 0");
    CHECK_EQ_U(DacWaveform_GetFrequency(DAC_BANK_A), 100000U, "GetFrequency refleja 100 kHz");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_B, 1U) == HAL_OK, "SET 1 Hz -> HAL_OK");
    CHECK_EQ_U(s_htimB.PSC, 457U,   "1 Hz -> PSC (banco B)");
    CHECK_EQ_U(s_htimB.ARR, 65501U, "1 Hz -> ARR (banco B)");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 300000U) == HAL_OK, "SET 300 kHz -> HAL_OK");
    CHECK_EQ_U(s_htimA.PSC, 0U,  "300 kHz -> PSC");
    CHECK_EQ_U(s_htimA.ARR, 99U, "300 kHz -> ARR");

    /* Limite superior exacto: 300000 pasa, 300001 se rechaza sin tocar nada. */
    uint32_t psc_prev = s_htimA.PSC;
    uint32_t arr_prev = s_htimA.ARR;
    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, DAC_FREQ_MAX_HZ + 1U) == HAL_ERROR,
          "300001 Hz -> HAL_ERROR");
    CHECK_EQ_U(s_htimA.PSC, psc_prev, "rechazo no toca TIMx->PSC");
    CHECK_EQ_U(s_htimA.ARR, arr_prev, "rechazo no toca TIMx->ARR");
    CHECK_EQ_U(DacWaveform_GetFrequency(DAC_BANK_A), 300000U, "GetFrequency intacto tras rechazo");

    /* 0 Hz por el API publico = parar el banco (no "sin division"). */
    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 0U) == HAL_OK, "0 Hz -> parada -> HAL_OK");
    CHECK_EQ_U(DacWaveform_GetFrequency(DAC_BANK_A), 0U, "banco parado -> GetFrequency 0");

    /* Banco fuera de rango. */
    CHECK(DacWaveform_SetFrequency((DacBank_t)5, 100000U) == HAL_ERROR, "banco invalido -> HAL_ERROR");

    /* Un fallo del periferico (DMA) se propaga y el banco no queda "running". */
    dac_setup();
    g_hal.dac_start_dma_ret = HAL_ERROR;
    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 100000U) == HAL_ERROR, "fallo de HAL_DAC_Start_DMA se propaga");
    CHECK_EQ_U(DacWaveform_GetFrequency(DAC_BANK_A), 0U, "tras fallo de DMA el banco no cuenta como activo");
}


/* ================================================================== *
 *  7. Onda centrada en VBIAS y aparcado del canal
 * ================================================================== */
static bool parked(int i)
{
    uint32_t ten = DAC_CR_TEN1 << (i == 0 ? 0 : 16);
    return g_hal.en[i] && !g_hal.dma[i] && g_hal.dhr[i] == DAC_CODE_MID &&
           (s_dac_regs.CR & ten) == 0U;
}

static void test_parking(void)
{
    printf("[test] onda centrada en VBIAS y aparcado del canal\n");

    CHECK_EQ_U(DAC_CODE_MID, 2048U, "VBIAS = mitad de escala (ratiometrico)");
    CHECK_EQ_U(DAC_CODE_HIGH - DAC_CODE_MID, 621U, "excursion positiva 621 codigos");
    CHECK_EQ_U(DAC_CODE_MID - DAC_CODE_LOW, 621U, "excursion negativa simetrica");
    CHECK_EQ_U(DAC_DIF_PEAK_MV, 500U, "621 * 3300 / 4096 = 500 mV de pico en Dif");
    CHECK_EQ_U(DAC_CODE_HIGH, 2669U, "nivel alto 2669 (2,15 V con VREF = 3,3 V)");
    CHECK_EQ_U(DAC_CODE_LOW, 1427U, "nivel bajo 1427 (1,15 V con VREF = 3,3 V)");

    dac_setup();
    CHECK(parked(0) && parked(1), "Init: ambos canales aparcados en 2048, habilitados, sin TRGO");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 100000U) == HAL_OK, "arranca banco A");
    CHECK(g_hal.ten_at_start_dma[0], "al arrancar la onda, TEN1 vuelve a estar activo");
    CHECK(g_hal.dma[0] && g_hal.en[0], "banco A en marcha por DMA");
    CHECK_EQ_U(g_hal.dma_len[0], 4U, "buffer de 4 muestras por periodo");
    CHECK(g_hal.dma_buf[0][0] == DAC_CODE_HIGH && g_hal.dma_buf[0][1] == DAC_CODE_LOW &&
          g_hal.dma_buf[0][2] == DAC_CODE_MID  && g_hal.dma_buf[0][3] == DAC_CODE_MID,
          "pulso bifasico con reposo: HIGH (T/4), LOW (T/4), MID (T/2)");
    {
        /* Dif medio nulo: la fase positiva y la negativa se compensan y el
         * reposo no aporta continua. */
        int32_t sum = 0;
        for (uint32_t k = 0U; k < g_hal.dma_len[0]; k++) {
            sum += (int32_t)g_hal.dma_buf[0][k] - (int32_t)DAC_CODE_MID;
        }
        CHECK_EQ_U(sum == 0, 1, "valor medio del pulso = VBIAS (sin continua)");
    }
    CHECK(parked(1), "el banco B sigue aparcado");

    CHECK(DacWaveform_Stop(DAC_BANK_A) == HAL_OK, "Stop banco A");
    CHECK(parked(0), "Stop: aparcado en 2048 (no deshabilitado)");
    CHECK_EQ_U(g_hal.tim_stop_calls > 0, 1, "timer parado");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_B, 1000U) == HAL_OK, "arranca banco B");
    CHECK(g_hal.ten_at_start_dma[1], "TEN2 activo al arrancar el banco B");
    CHECK(g_hal.dma_len[1] == 4U && g_hal.dma_buf[1][0] == DAC_CODE_HIGH &&
          g_hal.dma_buf[1][1] == DAC_CODE_LOW && g_hal.dma_buf[1][2] == DAC_CODE_MID &&
          g_hal.dma_buf[1][3] == DAC_CODE_MID, "banco B: mismo buffer de 4 muestras");
    CHECK(DacWaveform_SetFrequency(DAC_BANK_B, 0U) == HAL_OK, "0 Hz en banco B");
    CHECK(parked(1), "0 Hz: aparcado en 2048");

    dac_setup();
    g_hal.dac_start_dma_ret = HAL_ERROR;
    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 100000U) == HAL_ERROR, "fallo de DMA");
    CHECK(parked(0), "tras un fallo de DMA el canal no queda flotante: aparcado");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== Arnes de pruebas de dac_waveform (calculo PSC/ARR) ===\n");

    test_reference_points();
    test_limits_and_zero();
    test_band_sweep();
    test_decade_error();
    test_public_api();
    test_parking();

    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
