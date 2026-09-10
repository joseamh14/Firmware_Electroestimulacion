/**
 ******************************************************************************
 * @file    test_ad5144.c
 * @brief   Arnes de pruebas de escritorio para BSP/Src/ad5144.c (driver del
 *          potenciometro digital cuadruple AD5144 sobre I2C).
 *
 * No necesita el target ni STM32CubeIDE: se incluye ad5144.c entero (para
 * alcanzar la funcion estatica BuildCommandByte) y se enlaza contra los
 * mocks-espia de la HAL de I2C de mocks.c. Sin hardware real: cada
 * transaccion I2C se captura en g_spy (direccion, byte 0, byte 1, tamano,
 * timeout) y se puede forzar el retorno de HAL_I2C_IsDeviceReady y de
 * HAL_I2C_Master_Transmit por llamada.
 *
 * Formato de trama que se verifica (datasheet AD5124/AD5144/AD5144A Rev. B,
 * Tabla 20 "Advance Commands Operation Truth Table", Comando 1):
 *   byte 0 = [C3 C2 C1 C0 | A3 A2 A1 A0]
 *            control 0001 = "Write contents of serial register data to RDAC"
 *            direccion 0..3 = RDAC1..RDAC4 (canal A..D)
 *   byte 1 = [D7..D0] = codigo de 8 bits del wiper (0..255, AD5144 = 256 taps)
 *
 * La constante del driver AD5144_CMD_WRITE_RDAC = 0x01 se corresponde con
 * ese Comando 1 (nibble de control 0001): confirmado contra la tabla del
 * fabricante, no asumido.
 *
 * Cubre:
 *   1. AD5144_Init: punteros NULL, init correcto de U1 (0x2B) y U2 (0x28),
 *      shadow[] a cero, Trials/Timeout del sondeo, propagacion del fallo de
 *      HAL_I2C_IsDeviceReady.
 *   2. AD5144_SetChannelRaw: punteros NULL, trama exacta (byte 0 y byte 1)
 *      para los cuatro canales y codigos limite (0 y 255), el shadow[] se
 *      actualiza solo si la transaccion devuelve HAL_OK.
 *   3. AD5144_SetChannelMillivolts: full_scale_mv = 0 -> HAL_ERROR, clamp por
 *      encima de fondo de escala, redondeo al alza y a la baja, el codigo
 *      nunca supera 255, dev NULL.
 *   4. AD5144_AllChannelsZero: escribe 0 en A,B,C,D en orden, sigue con los
 *      canales restantes si uno falla y devuelve el estado del ULTIMO fallo
 *      (comportamiento actual del driver; ver HALLAZGO en el README).
 ******************************************************************************
 */
#include "mock_hal.h"

#include <stdio.h>
#include <string.h>

/* El driver bajo prueba, sin modificar. */
#include "ad5144.c"

/* ------------------------------------------------------------------ *
 *  Mini framework de asserts (mismo estilo que test/dac_waveform/)
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

/* Direcciones de los dos chips de la placa (strap del pin ADDR). */
#define ADDR_U1  AD5144_I2C_ADDR_ADDR_VLOG   /* 0x2B, canales 1-4 */
#define ADDR_U2  AD5144_I2C_ADDR_ADDR_GND    /* 0x28, canales 5-8 */

/* Direccion de 8 bits tal y como la pasa el driver (7 bits << 1). */
static uint16_t addr8(uint8_t addr7) { return (uint16_t)(addr7 << 1); }

/* Comprueba la trama capturada en la transaccion idx. */
static void expect_frame(int idx, uint8_t addr7, uint8_t b0, uint8_t b1, const char *what)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: byte0", what);
    CHECK_EQ_U(g_spy.tx_b0[idx], b0, msg);
    snprintf(msg, sizeof(msg), "%s: byte1", what);
    CHECK_EQ_U(g_spy.tx_b1[idx], b1, msg);
    snprintf(msg, sizeof(msg), "%s: direccion I2C (7b << 1)", what);
    CHECK_EQ_U(g_spy.tx_addr[idx], addr8(addr7), msg);
    snprintf(msg, sizeof(msg), "%s: Size = 2", what);
    CHECK_EQ_U(g_spy.tx_size[idx], 2U, msg);
    snprintf(msg, sizeof(msg), "%s: Timeout = AD5144_I2C_TIMEOUT_MS", what);
    CHECK_EQ_U(g_spy.tx_timeout[idx], AD5144_I2C_TIMEOUT_MS, msg);
}

/* ================================================================== *
 *  0. Cruce con la datasheet: BuildCommandByte / Comando 1 (Write RDAC)
 * ================================================================== */
static void test_command_encoding(void)
{
    printf("[test] codificacion del byte de comando (datasheet Tabla 20, Comando 1)\n");

    /* AD5144_CMD_WRITE_RDAC es privado del .c; se ve por el #include. */
    CHECK_EQ_U(AD5144_CMD_WRITE_RDAC, 0x01U, "instruccion Write RDAC = 0x1 (nibble 0001)");

    CHECK_EQ_U(BuildCommandByte(AD5144_CMD_WRITE_RDAC, AD5144_CH_A), 0x10U, "canal A -> 0x10");
    CHECK_EQ_U(BuildCommandByte(AD5144_CMD_WRITE_RDAC, AD5144_CH_B), 0x11U, "canal B -> 0x11");
    CHECK_EQ_U(BuildCommandByte(AD5144_CMD_WRITE_RDAC, AD5144_CH_C), 0x12U, "canal C -> 0x12");
    CHECK_EQ_U(BuildCommandByte(AD5144_CMD_WRITE_RDAC, AD5144_CH_D), 0x13U, "canal D -> 0x13");

    /* El nibble alto (control) es 0001 para los cuatro canales. */
    for (uint8_t ch = 0U; ch < 4U; ch++) {
        CHECK_EQ_U(BuildCommandByte(AD5144_CMD_WRITE_RDAC, ch) & 0xF0U, 0x10U,
                   "nibble de control siempre 0001");
        CHECK_EQ_U(BuildCommandByte(AD5144_CMD_WRITE_RDAC, ch) & 0x0FU, ch,
                   "nibble bajo = direccion de canal");
    }
}

/* ================================================================== *
 *  1. AD5144_Init
 * ================================================================== */
static void test_init(void)
{
    printf("[test] AD5144_Init: NULL, init de U1/U2, shadow, sondeo, propagacion de fallo\n");

    AD5144_Handle_t dev;
    I2C_HandleTypeDef hi2c;

    /* --- Punteros NULL: HAL_ERROR y ni una transaccion al bus --- */
    Ad5144Spy_Reset();
    CHECK(AD5144_Init(NULL, &hi2c, ADDR_U1) == HAL_ERROR, "dev NULL -> HAL_ERROR");
    CHECK(AD5144_Init(&dev, NULL, ADDR_U1) == HAL_ERROR, "hi2c NULL -> HAL_ERROR");
    CHECK(AD5144_Init(NULL, NULL, ADDR_U1) == HAL_ERROR, "ambos NULL -> HAL_ERROR");
    CHECK_EQ_U(g_spy.ready_calls, 0, "Init con NULL no sondea el bus");
    CHECK_EQ_U(g_spy.tx_calls, 0, "Init con NULL no transmite");

    /* --- Init correcto de U1 (0x2B) --- */
    Ad5144Spy_Reset();
    memset(&dev, 0xAA, sizeof(dev));   /* shadow con basura antes de Init */
    g_spy.ready_ret = HAL_OK;
    CHECK(AD5144_Init(&dev, &hi2c, ADDR_U1) == HAL_OK, "U1: Init -> HAL_OK");
    CHECK(dev.hi2c == &hi2c, "U1: guarda el handle I2C");
    CHECK_EQ_U(dev.addr7, ADDR_U1, "U1: guarda addr7 = 0x2B");
    CHECK_EQ_U(g_spy.ready_calls, 1, "U1: sondea el bus una vez");
    CHECK_EQ_U(g_spy.ready_addr, addr8(ADDR_U1), "U1: sondea en 0x2B << 1 = 0x56");
    CHECK_EQ_U(g_spy.ready_trials, 2U, "U1: Trials = 2");
    CHECK_EQ_U(g_spy.ready_timeout, AD5144_I2C_TIMEOUT_MS, "U1: Timeout del sondeo");
    CHECK_EQ_U(g_spy.tx_calls, 0, "U1: Init no escribe ningun RDAC");
    CHECK_EQ_U(dev.shadow[0], 0U, "U1: shadow[0] = 0 tras Init");
    CHECK_EQ_U(dev.shadow[1], 0U, "U1: shadow[1] = 0 tras Init");
    CHECK_EQ_U(dev.shadow[2], 0U, "U1: shadow[2] = 0 tras Init");
    CHECK_EQ_U(dev.shadow[3], 0U, "U1: shadow[3] = 0 tras Init");

    /* --- Init correcto de U2 (0x28) --- */
    Ad5144Spy_Reset();
    g_spy.ready_ret = HAL_OK;
    CHECK(AD5144_Init(&dev, &hi2c, ADDR_U2) == HAL_OK, "U2: Init -> HAL_OK");
    CHECK_EQ_U(dev.addr7, ADDR_U2, "U2: guarda addr7 = 0x28");
    CHECK_EQ_U(g_spy.ready_addr, addr8(ADDR_U2), "U2: sondea en 0x28 << 1 = 0x50");

    /* Las dos constantes de direccion de la cabecera son las esperadas. */
    CHECK_EQ_U(AD5144_I2C_ADDR_ADDR_VLOG, 0x2BU, "AD5144_I2C_ADDR_ADDR_VLOG = 0x2B");
    CHECK_EQ_U(AD5144_I2C_ADDR_ADDR_GND, 0x28U, "AD5144_I2C_ADDR_ADDR_GND = 0x28");

    /* --- Propagacion del fallo de HAL_I2C_IsDeviceReady --- */
    static const HAL_StatusTypeDef fails[] = { HAL_ERROR, HAL_BUSY, HAL_TIMEOUT };
    for (size_t i = 0U; i < sizeof(fails) / sizeof(fails[0]); i++) {
        Ad5144Spy_Reset();
        memset(&dev, 0x5A, sizeof(dev));
        g_spy.ready_ret = fails[i];
        char msg[80];
        snprintf(msg, sizeof(msg), "sondeo devuelve %d -> Init propaga el mismo estado", (int)fails[i]);
        CHECK(AD5144_Init(&dev, &hi2c, ADDR_U1) == fails[i], msg);
        /* HALLAZGO (menor): aun con el sondeo fallido, Init deja el handle
         * completamente poblado -- hi2c y addr7 escritos, shadow a cero. Un
         * llamante que ignore el valor de retorno se queda con un handle de
         * aspecto usable. Ver README. */
        CHECK(dev.hi2c == &hi2c, "tras sondeo fallido, hi2c queda igualmente asignado");
        CHECK_EQ_U(dev.shadow[0], 0U, "tras sondeo fallido, shadow[0] = 0");
        CHECK_EQ_U(dev.shadow[3], 0U, "tras sondeo fallido, shadow[3] = 0");
    }
}

/* Init de conveniencia para el resto de bloques. */
static void init_ok(AD5144_Handle_t *dev, I2C_HandleTypeDef *hi2c, uint8_t addr7)
{
    Ad5144Spy_Reset();
    g_spy.ready_ret = HAL_OK;
    CHECK(AD5144_Init(dev, hi2c, addr7) == HAL_OK, "precondicion: Init -> HAL_OK");
    Ad5144Spy_Reset();   /* limpia el sondeo, deja el arnes listo para las escrituras */
}

/* ================================================================== *
 *  2. AD5144_SetChannelRaw
 * ================================================================== */
static void test_set_channel_raw(void)
{
    printf("[test] AD5144_SetChannelRaw: NULL, trama exacta por canal, shadow condicionado a HAL_OK\n");

    AD5144_Handle_t dev;
    I2C_HandleTypeDef hi2c;

    /* --- Punteros NULL --- */
    Ad5144Spy_Reset();
    CHECK(AD5144_SetChannelRaw(NULL, AD5144_CH_A, 100U) == HAL_ERROR, "dev NULL -> HAL_ERROR");
    AD5144_Handle_t sin_bus;
    memset(&sin_bus, 0, sizeof(sin_bus));   /* hi2c = NULL */
    CHECK(AD5144_SetChannelRaw(&sin_bus, AD5144_CH_A, 100U) == HAL_ERROR, "dev->hi2c NULL -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "con puntero NULL no se transmite nada");

    /* --- Trama exacta para los cuatro canales a un codigo medio --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    static const AD5144_Channel_t chs[4] = { AD5144_CH_A, AD5144_CH_B, AD5144_CH_C, AD5144_CH_D };
    static const uint8_t expect_b0[4]    = { 0x10U, 0x11U, 0x12U, 0x13U };
    for (int i = 0; i < 4; i++) {
        int before = g_spy.tx_calls;
        CHECK(AD5144_SetChannelRaw(&dev, chs[i], 0x7FU) == HAL_OK, "canal medio: SetChannelRaw -> HAL_OK");
        CHECK_EQ_U(g_spy.tx_calls, before + 1, "una unica transaccion por escritura");
        expect_frame(before, ADDR_U1, expect_b0[i], 0x7FU, "canal A..D @ code 0x7F");
        CHECK_EQ_U(dev.shadow[chs[i]], 0x7FU, "shadow[canal] = code tras HAL_OK");
    }

    /* --- Codigos limite 0 y 255 --- */
    init_ok(&dev, &hi2c, ADDR_U2);
    CHECK(AD5144_SetChannelRaw(&dev, AD5144_CH_A, 0U) == HAL_OK, "code 0: HAL_OK");
    expect_frame(0, ADDR_U2, 0x10U, 0x00U, "canal A @ code 0");
    CHECK_EQ_U(dev.shadow[AD5144_CH_A], 0U, "shadow = 0");
    CHECK(AD5144_SetChannelRaw(&dev, AD5144_CH_D, 255U) == HAL_OK, "code 255: HAL_OK");
    expect_frame(1, ADDR_U2, 0x13U, 0xFFU, "canal D @ code 255");
    CHECK_EQ_U(dev.shadow[AD5144_CH_D], 255U, "shadow = 255");

    /* --- El shadow[] NO se actualiza si la transaccion no devuelve HAL_OK --- */
    static const HAL_StatusTypeDef bad[] = { HAL_ERROR, HAL_BUSY, HAL_TIMEOUT };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); i++) {
        init_ok(&dev, &hi2c, ADDR_U1);
        CHECK(AD5144_SetChannelRaw(&dev, AD5144_CH_B, 0x42U) == HAL_OK, "primero un valor bueno");
        CHECK_EQ_U(dev.shadow[AD5144_CH_B], 0x42U, "shadow = 0x42 tras HAL_OK");

        g_spy.tx_ret = bad[i];
        char msg[80];
        snprintf(msg, sizeof(msg), "transaccion devuelve %d -> SetChannelRaw propaga", (int)bad[i]);
        CHECK(AD5144_SetChannelRaw(&dev, AD5144_CH_B, 0x99U) == bad[i], msg);
        CHECK_EQ_U(dev.shadow[AD5144_CH_B], 0x42U, "shadow intacto tras fallo (no 0x99)");
    }
}

/* ================================================================== *
 *  3. AD5144_SetChannelMillivolts
 * ================================================================== */

/* Codigo de wiper que espera la formula del driver:
 *   round(target_mv * 255 / full_scale_mv), con target clampeado a fondo de
 *   escala. */
static uint32_t expected_code(uint16_t target_mv, uint16_t full_scale_mv)
{
    if (target_mv > full_scale_mv) {
        target_mv = full_scale_mv;
    }
    uint32_t c = ((uint32_t)target_mv * 255U + (full_scale_mv / 2U)) / full_scale_mv;
    if (c > 255U) {
        c = 255U;
    }
    return c;
}

static void test_set_channel_millivolts(void)
{
    printf("[test] AD5144_SetChannelMillivolts: full_scale 0, clamp, redondeo, tope 255, dev NULL\n");

    AD5144_Handle_t dev;
    I2C_HandleTypeDef hi2c;

    /* --- full_scale_mv = 0 -> HAL_ERROR, sin tocar el bus --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_A, 100U, 0U) == HAL_ERROR,
          "full_scale_mv = 0 -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "full_scale 0 no transmite");

    /* --- dev NULL -> HAL_ERROR aunque full_scale sea valido --- */
    Ad5144Spy_Reset();
    CHECK(AD5144_SetChannelMillivolts(NULL, AD5144_CH_A, 100U, 1000U) == HAL_ERROR,
          "dev NULL -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "dev NULL no transmite");

    /* --- Clamp: target por encima de fondo de escala se limita, no se
     *     escala fuera de rango --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_A, 1500U, 1000U) == HAL_OK,
          "target 1500 > fs 1000: HAL_OK");
    expect_frame(0, ADDR_U1, 0x10U, 255U, "target > fs -> code 255 (clamp)");
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_A, 1000U, 1000U) == HAL_OK,
          "target == fs: HAL_OK");
    expect_frame(1, ADDR_U1, 0x10U, 255U, "target == fs -> code 255");

    /* --- Redondeo en varios puntos del rango (fs = 1000 mV) --------------
     *   target : codigo exacto -> redondeado
     *      1   :  0.255  -> 0   (a la baja)
     *      2   :  0.510  -> 1   (al alza)
     *     99   : 25.245  -> 25  (a la baja)
     *    100   : 25.500  -> 26  (al alza, empate)
     *    500   : 127.500 -> 128 (al alza, empate)
     *    997   : 254.235 -> 254 (a la baja)
     *    999   : 254.745 -> 255 (al alza) */
    struct { uint16_t mv; uint8_t code; const char *nota; } pts[] = {
        {   0U,   0U, "0 mV -> 0" },
        {   1U,   0U, "1 mV (0.255) -> 0 [a la baja]" },
        {   2U,   1U, "2 mV (0.510) -> 1 [al alza]" },
        {  99U,  25U, "99 mV (25.245) -> 25 [a la baja]" },
        { 100U,  26U, "100 mV (25.500) -> 26 [al alza, empate]" },
        { 500U, 128U, "500 mV (127.500) -> 128 [al alza, empate]" },
        { 997U, 254U, "997 mV (254.235) -> 254 [a la baja]" },
        { 999U, 255U, "999 mV (254.745) -> 255 [al alza]" },
        {1000U, 255U, "1000 mV -> 255" },
    };
    for (size_t i = 0U; i < sizeof(pts) / sizeof(pts[0]); i++) {
        init_ok(&dev, &hi2c, ADDR_U1);
        CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_C, pts[i].mv, 1000U) == HAL_OK, pts[i].nota);
        expect_frame(0, ADDR_U1, 0x12U, pts[i].code, pts[i].nota);
        CHECK_EQ_U(dev.shadow[AD5144_CH_C], pts[i].code, "shadow = codigo redondeado");
        /* referencia cruzada con la formula reimplementada en el arnes */
        CHECK_EQ_U(pts[i].code, expected_code(pts[i].mv, 1000U), "codigo esperado coincide con la formula");
    }

    /* --- fs = 2000 mV (limite de software del proyecto) --- */
    init_ok(&dev, &hi2c, ADDR_U2);
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_A, 2000U, 2000U) == HAL_OK, "fs 2000: target 2000 -> 255");
    expect_frame(0, ADDR_U2, 0x10U, 255U, "fs 2000, target 2000");
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_A, 1000U, 2000U) == HAL_OK, "fs 2000: target 1000 -> 128");
    expect_frame(1, ADDR_U2, 0x10U, 128U, "fs 2000, target 1000 (medio)");

    /* --- El codigo resultante NUNCA supera 255, en todo el barrido del rango,
     *     para varias escalas (incluye fs impar). La rama 'if (code > 255)'
     *     del driver es, con estas entradas, inalcanzable: el clamp previo de
     *     target ya acota el maximo a 255. Ver README (HALLAZGO menor).
     *     Barrido agregado: se cuentan las discrepancias y se afirman al
     *     final (mismo estilo que el barrido de banda de dac_waveform). --- */
    static const uint16_t scales[] = { 100U, 333U, 1000U, 1200U, 2000U, 65535U };
    int sweep_err_ret   = 0;   /* alguna llamada no devolvio HAL_OK        */
    int sweep_over_255  = 0;   /* algun codigo > 255                       */
    int sweep_mismatch  = 0;   /* codigo != formula del arnes              */
    int sweep_clamp_bad = 0;   /* target >= fs y codigo != 255             */
    unsigned long sweep_points = 0UL;
    init_ok(&dev, &hi2c, ADDR_U1);
    for (size_t s = 0U; s < sizeof(scales) / sizeof(scales[0]); s++) {
        uint16_t fs   = scales[s];
        uint32_t step = (fs > 400U) ? (fs / 400U) : 1U;   /* barrido muestreado */
        for (uint32_t mv = 0U; mv <= (uint32_t)fs + step; mv += step) {
            uint16_t t = (mv > 0xFFFFU) ? 0xFFFFU : (uint16_t)mv;
            Ad5144Spy_Reset();
            g_spy.ready_ret = HAL_OK;
            if (AD5144_SetChannelMillivolts(&dev, AD5144_CH_A, t, fs) != HAL_OK) { sweep_err_ret++; }
            if (g_spy.tx_b1[0] > 255U) { sweep_over_255++; }   /* imposible en uint8_t, explicito */
            if ((uint32_t)g_spy.tx_b1[0] != expected_code(t, fs)) { sweep_mismatch++; }
            if (t >= fs && g_spy.tx_b1[0] != 255U) { sweep_clamp_bad++; }
            sweep_points++;
        }
    }
    printf("   barrido mV->code: %lu puntos sobre %lu escalas\n",
           sweep_points, (unsigned long)(sizeof(scales) / sizeof(scales[0])));
    CHECK(sweep_points > 2000UL, "el barrido cubre miles de puntos");
    CHECK_EQ_U(sweep_err_ret, 0,   "toda llamada del barrido devuelve HAL_OK");
    CHECK_EQ_U(sweep_over_255, 0,  "ningun codigo supera 255 en todo el barrido");
    CHECK_EQ_U(sweep_mismatch, 0,  "todo el barrido coincide con la formula del driver");
    CHECK_EQ_U(sweep_clamp_bad, 0, "target >= fondo de escala -> code 255 en todo el barrido");
}

/* ================================================================== *
 *  4. AD5144_AllChannelsZero
 * ================================================================== */
static void test_all_channels_zero(void)
{
    printf("[test] AD5144_AllChannelsZero: orden A,B,C,D, no corta ante fallo, devuelve el ultimo fallo\n");

    AD5144_Handle_t dev;
    I2C_HandleTypeDef hi2c;

    /* --- Camino feliz: 4 escrituras de 0 en orden A,B,C,D --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    /* ensuciamos el shadow[] con escrituras previas */
    for (int i = 0; i < 4; i++) {
        AD5144_SetChannelRaw(&dev, (AD5144_Channel_t)i, (uint8_t)(0x50U + i));
    }
    Ad5144Spy_Reset();
    CHECK(AD5144_AllChannelsZero(&dev) == HAL_OK, "todo OK -> HAL_OK");
    CHECK_EQ_U(g_spy.tx_calls, 4, "exactamente 4 transacciones");
    expect_frame(0, ADDR_U1, 0x10U, 0x00U, "AllZero paso 1 = canal A a 0");
    expect_frame(1, ADDR_U1, 0x11U, 0x00U, "AllZero paso 2 = canal B a 0");
    expect_frame(2, ADDR_U1, 0x12U, 0x00U, "AllZero paso 3 = canal C a 0");
    expect_frame(3, ADDR_U1, 0x13U, 0x00U, "AllZero paso 4 = canal D a 0");
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_U(dev.shadow[i], 0U, "shadow[canal] = 0 tras AllChannelsZero");
    }

    /* --- Un fallo a mitad NO corta el barrido y el estado devuelto es el
     *     del ULTIMO fallo (comportamiento actual; enmascara fallos
     *     intermedios -> HALLAZGO documentado en el README) --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    for (int i = 0; i < 4; i++) {
        AD5144_SetChannelRaw(&dev, (AD5144_Channel_t)i, 0xEEU);   /* shadow != 0 en los 4 */
    }
    Ad5144Spy_Reset();
    g_spy.tx_ret_seq[1] = HAL_TIMEOUT;   /* canal B falla */
    g_spy.tx_ret_seq[3] = HAL_BUSY;      /* canal D falla, mas tarde */
    HAL_StatusTypeDef r = AD5144_AllChannelsZero(&dev);
    CHECK_EQ_U(g_spy.tx_calls, 4, "sigue intentando los 4 canales pese a los fallos");
    CHECK(r == HAL_BUSY, "devuelve el ultimo fallo (HAL_BUSY), no el primero (HAL_TIMEOUT)");
    CHECK_EQ_U(dev.shadow[AD5144_CH_A], 0U,    "canal A OK -> shadow 0");
    CHECK_EQ_U(dev.shadow[AD5144_CH_B], 0xEEU, "canal B fallo -> shadow intacto");
    CHECK_EQ_U(dev.shadow[AD5144_CH_C], 0U,    "canal C OK -> shadow 0");
    CHECK_EQ_U(dev.shadow[AD5144_CH_D], 0xEEU, "canal D fallo -> shadow intacto");

    /* --- Si solo falla el primer canal, se devuelve ese estado --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    Ad5144Spy_Reset();
    g_spy.tx_ret_seq[0] = HAL_ERROR;
    CHECK(AD5144_AllChannelsZero(&dev) == HAL_ERROR, "solo falla A -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 4, "aun asi intenta los 4 canales");

    /* --- dev NULL: cada SetChannelRaw interno devuelve HAL_ERROR, sin bus --- */
    Ad5144Spy_Reset();
    CHECK(AD5144_AllChannelsZero(NULL) == HAL_ERROR, "dev NULL -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "dev NULL no transmite");

    /* --- dev->hi2c NULL: idem --- */
    AD5144_Handle_t sin_bus;
    memset(&sin_bus, 0, sizeof(sin_bus));
    Ad5144Spy_Reset();
    CHECK(AD5144_AllChannelsZero(&sin_bus) == HAL_ERROR, "dev->hi2c NULL -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "dev->hi2c NULL no transmite");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== Arnes de pruebas de ad5144 (driver AD5144 I2C) ===\n");

    test_command_encoding();
    test_init();
    test_set_channel_raw();
    test_set_channel_millivolts();
    test_all_channels_zero();

    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
