/**
 ******************************************************************************
 * @file    test_dac_waveform.c
 * @brief   Arnes de pruebas de escritorio para el calculo de PSC/ARR de
 *          BSP/Src/dac_waveform.c.
 *
 * La funcion ComputeTimerParams() es aritmetica pura: a partir de una
 * frecuencia de DISPARO (2x la de la onda, por las 2 muestras/periodo del
 * buffer DMA) busca el par (prescaler, autorecarga) que mejor la aproxima
 * sin que ARR+1 se salga de 16 bits. No toca ningun registro, asi que se
 * prueba sin mocks de comportamiento.
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
 *   1. Los tres puntos verificados a mano en el Anexo de calculos:
 *      1 Hz -> PSC=915/ARR=65501, 100 kHz -> 0/599, 300 kHz -> 0/199.
 *   2. Limite inferior (1 Hz) y rechazo de 0 Hz (no se interpreta como
 *      "sin division"): salida sin tocar, retorno false.
 *   3. Techo aritmetico exacto sin prescaler (trigger = Fclk -> ARR=0). El
 *      limite superior UTIL (300 kHz) se prueba por el API publico (punto 6).
 *   4. Barrido de toda la banda 1 Hz - 300 kHz: nunca rechaza, ARR/PSC
 *      siempre en 16 bits (no hay desbordamiento silencioso del registro),
 *      error relativo acotado.
 *   5. Barrido por decadas (10/100/1k/10k Hz): error relativo dentro del
 *      orden de magnitud del Anexo (~3e-4 %, es decir ~3 ppm).
 *   6. Via API publico DacWaveform_SetFrequency: el par (PSC, ARR) llega al
 *      timer, el limite superior se rechaza sin tocar el estado anterior,
 *      0 Hz para el banco y un fallo de DMA se propaga.
 *   7. Onda centrada en VBIAS y aparcado (revision de esquematicos de
 *      octubre de 2026): codigos 2048 +/- 690, amplitud de Dif de 556 mV;
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
 *   err = |Fclk - 2*f*D| / (2*f*D),   D = (psc+1)*(arr+1)
 * (el timer dispara a Fclk/D y la onda sale a la mitad). */
static uint64_t rel_err_ppm(uint32_t wave_hz, uint32_t psc, uint32_t arr)
{
    uint64_t den  = 2ULL * (uint64_t)wave_hz * total_div(psc, arr);
    uint64_t fclk = (uint64_t)DAC_TRIGGER_CLOCK_HZ;
    uint64_t num  = (fclk > den) ? (fclk - den) : (den - fclk);
    return (num * 1000000ULL) / den;
}

/* Frecuencia de onda reconstruida, en milihercios, para los volcados. */
static uint64_t wave_mhz(uint32_t psc, uint32_t arr)
{
    return ((uint64_t)DAC_TRIGGER_CLOCK_HZ * 1000ULL) / (2ULL * total_div(psc, arr));
}

/* ComputeTimerParams trabaja sobre la frecuencia de DISPARO; este wrapper
 * lo llama en terminos de la frecuencia de ONDA (la que ve el usuario). */
static bool params_for_wave(uint32_t wave_hz, uint32_t *psc, uint32_t *arr)
{
    return ComputeTimerParams(wave_hz * 2U, psc, arr);
}

/* ================================================================== *
 *  1. Puntos del Anexo de calculos
 * ================================================================== */
static void test_reference_points(void)
{
    printf("[test] puntos verificados a mano (Anexo de calculos)\n");
    uint32_t psc, arr;

    CHECK(params_for_wave(1U, &psc, &arr), "1 Hz: calculable");
    CHECK_EQ_U(psc, 915U,   "1 Hz -> PSC=915");
    CHECK_EQ_U(arr, 65501U, "1 Hz -> ARR=65501");

    CHECK(params_for_wave(100000U, &psc, &arr), "100 kHz: calculable");
    CHECK_EQ_U(psc, 0U,   "100 kHz -> PSC=0");
    CHECK_EQ_U(arr, 599U, "100 kHz -> ARR=599");

    CHECK(params_for_wave(300000U, &psc, &arr), "300 kHz: calculable");
    CHECK_EQ_U(psc, 0U,   "300 kHz -> PSC=0");
    CHECK_EQ_U(arr, 199U, "300 kHz -> ARR=199");
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
        if (!ComputeTimerParams(f * 2U, &psc, &arr)) {
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
    /* La resolucion cerca de 300 kHz es de ~0.5 % (ARR ~= 200): el barrido
     * solo garantiza que el buscador converge y acota el error, no la
     * exactitud fina -- esa se mide en los puntos de decada. */
    CHECK(worst_ppm < 6000U, "error relativo de banda acotado (< 6000 ppm)");
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

    /* El Anexo cita ~3e-4 %% (~3 ppm) de peor caso sobre puntos con division
     * exacta (1 Hz / 100 kHz / 300 kHz). El barrido mete 10 Hz, cuyo error
     * real es ~6 ppm; ambos quedan holgados por debajo de 10 ppm (1e-3 %%). */
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
    CHECK_EQ_U(s_htimA.ARR, 599U, "100 kHz -> TIMx->ARR");
    CHECK_EQ_U(s_htimA.CNT, 0U,   "reconfig pone TIMx->CNT a 0");
    CHECK_EQ_U(DacWaveform_GetFrequency(DAC_BANK_A), 100000U, "GetFrequency refleja 100 kHz");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_B, 1U) == HAL_OK, "SET 1 Hz -> HAL_OK");
    CHECK_EQ_U(s_htimB.PSC, 915U,   "1 Hz -> PSC (banco B)");
    CHECK_EQ_U(s_htimB.ARR, 65501U, "1 Hz -> ARR (banco B)");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 300000U) == HAL_OK, "SET 300 kHz -> HAL_OK");
    CHECK_EQ_U(s_htimA.PSC, 0U,   "300 kHz -> PSC");
    CHECK_EQ_U(s_htimA.ARR, 199U, "300 kHz -> ARR");

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
    CHECK_EQ_U(DAC_CODE_HIGH - DAC_CODE_MID, 690U, "excursion positiva 690 codigos");
    CHECK_EQ_U(DAC_CODE_MID - DAC_CODE_LOW, 690U, "excursion negativa simetrica");
    CHECK_EQ_U(DAC_DIF_PEAK_MV, 556U, "690 * 3300 / 4096 = 556 mV de pico en Dif");

    dac_setup();
    CHECK(parked(0) && parked(1), "Init: ambos canales aparcados en 2048, habilitados, sin TRGO");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_A, 100000U) == HAL_OK, "arranca banco A");
    CHECK(g_hal.ten_at_start_dma[0], "al arrancar la onda, TEN1 vuelve a estar activo");
    CHECK(g_hal.dma[0] && g_hal.en[0], "banco A en marcha por DMA");
    CHECK(g_hal.dma_len[0] == 2U && g_hal.dma_buf[0][0] == DAC_CODE_HIGH &&
          g_hal.dma_buf[0][1] == DAC_CODE_LOW, "buffer de 2 muestras HIGH/LOW");
    CHECK(parked(1), "el banco B sigue aparcado");

    CHECK(DacWaveform_Stop(DAC_BANK_A) == HAL_OK, "Stop banco A");
    CHECK(parked(0), "Stop: aparcado en 2048 (no deshabilitado)");
    CHECK_EQ_U(g_hal.tim_stop_calls > 0, 1, "timer parado");

    CHECK(DacWaveform_SetFrequency(DAC_BANK_B, 1000U) == HAL_OK, "arranca banco B");
    CHECK(g_hal.ten_at_start_dma[1], "TEN2 activo al arrancar el banco B");
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
