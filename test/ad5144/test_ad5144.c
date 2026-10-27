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
 *   1. AD5144_Init: punteros NULL, init correcto de U1 (0x28, ADDR=VLOGIC) y
 *      U2 (0x2B, ADDR=GND) segun la tabla 15 de la datasheet, shadow[] a la
 *      mitad de escala de encendido, Trials/Timeout del sondeo, propagacion
 *      del fallo de HAL_I2C_IsDeviceReady.
 *   2. AD5144_SetChannelRaw: punteros NULL, trama exacta (byte 0 y byte 1)
 *      para los cuatro canales y codigos limite (0 y 255), el shadow[] se
 *      actualiza solo si la transaccion devuelve HAL_OK.
 *   3. AD5144_CodeForMillivolts / AD5144_SetChannelMillivolts con el modelo
 *      de reostato en la realimentacion del inversor de ESA:
 *      |OUT| = in_peak * R_AW(D) / 2,5 kOhm, R_AW(D) = (256-D)/256*R_AB + R_W.
 *      Relacion inversa, objetivo inalcanzable -> HAL_ERROR sin escribir,
 *      por debajo del minimo -> codigo 255, error de redondeo <= medio paso
 *      en todo el barrido, resolucion <= 10 mV con la excursion del DAC.
 *   5. AD5144_StoreEepromCode / AD5144_StoreSafePowerUp: Comando 11 de la
 *      datasheet (0x80 | canal, dato), sondeo por ACK de 1 ms hasta 60 ms.
 *   4. AD5144_AllChannelsZero: escribe 255 (ganancia minima, posicion
 *      segura) en A,B,C,D en orden, sigue con los canales restantes si uno
 *      falla y devuelve el estado del ULTIMO fallo (ver HALLAZGO en el README).
 ******************************************************************************
 */
#include "mock_hal.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

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
#define ADDR_U1  AD5144_I2C_ADDR_ADDR_VLOG   /* 0x28, canales 1-4 (ADDR = 2V5) */
#define ADDR_U2  AD5144_I2C_ADDR_ADDR_GND    /* 0x2B, canales 5-8 (ADDR = GND) */

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

    /* --- Init correcto de U1 (0x28) --- */
    Ad5144Spy_Reset();
    memset(&dev, 0xAA, sizeof(dev));   /* shadow con basura antes de Init */
    g_spy.ready_ret = HAL_OK;
    CHECK(AD5144_Init(&dev, &hi2c, ADDR_U1) == HAL_OK, "U1: Init -> HAL_OK");
    CHECK(dev.hi2c == &hi2c, "U1: guarda el handle I2C");
    CHECK_EQ_U(dev.addr7, ADDR_U1, "U1: guarda addr7 = 0x28");
    CHECK_EQ_U(g_spy.ready_calls, 1, "U1: sondea el bus una vez");
    CHECK_EQ_U(g_spy.ready_addr, addr8(ADDR_U1), "U1: sondea en 0x28 << 1 = 0x50");
    CHECK_EQ_U(g_spy.ready_addr, 0x50U, "U1: direccion de 8 bits 0x50");
    CHECK_EQ_U(g_spy.ready_trials, 2U, "U1: Trials = 2");
    CHECK_EQ_U(g_spy.ready_timeout, AD5144_I2C_TIMEOUT_MS, "U1: Timeout del sondeo");
    CHECK_EQ_U(g_spy.tx_calls, 0, "U1: Init no escribe ningun RDAC");
    CHECK_EQ_U(dev.shadow[0], AD5144_CODE_POWERUP, "U1: shadow[0] = 128 (EEPROM de fabrica) tras Init");
    CHECK_EQ_U(dev.shadow[1], AD5144_CODE_POWERUP, "U1: shadow[1] = 128 (EEPROM de fabrica) tras Init");
    CHECK_EQ_U(dev.shadow[2], AD5144_CODE_POWERUP, "U1: shadow[2] = 128 (EEPROM de fabrica) tras Init");
    CHECK_EQ_U(dev.shadow[3], AD5144_CODE_POWERUP, "U1: shadow[3] = 128 (EEPROM de fabrica) tras Init");

    /* --- Init correcto de U2 (0x2B) --- */
    Ad5144Spy_Reset();
    g_spy.ready_ret = HAL_OK;
    CHECK(AD5144_Init(&dev, &hi2c, ADDR_U2) == HAL_OK, "U2: Init -> HAL_OK");
    CHECK_EQ_U(dev.addr7, ADDR_U2, "U2: guarda addr7 = 0x2B");
    CHECK_EQ_U(g_spy.ready_addr, addr8(ADDR_U2), "U2: sondea en 0x2B << 1 = 0x56");
    CHECK_EQ_U(g_spy.ready_addr, 0x56U, "U2: direccion de 8 bits 0x56");

    /* Las dos constantes de direccion de la cabecera son las de la tabla 15
     * de la datasheet (TSSOP-20): ADDR = VLOGIC -> 0101000, GND -> 0101011. */
    CHECK_EQ_U(AD5144_I2C_ADDR_ADDR_VLOG, 0x28U, "AD5144_I2C_ADDR_ADDR_VLOG = 0x28 (0101000)");
    CHECK_EQ_U(AD5144_I2C_ADDR_ADDR_GND, 0x2BU, "AD5144_I2C_ADDR_ADDR_GND = 0x2B (0101011)");
    CHECK(AD5144_I2C_ADDR_ADDR_VLOG != AD5144_I2C_ADDR_ADDR_GND, "U1 y U2 en direcciones distintas");

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
         * completamente poblado -- hi2c y addr7 escritos, shadow a 128. Un
         * llamante que ignore el valor de retorno se queda con un handle de
         * aspecto usable. Ver README. */
        CHECK(dev.hi2c == &hi2c, "tras sondeo fallido, hi2c queda igualmente asignado");
        CHECK_EQ_U(dev.shadow[0], AD5144_CODE_POWERUP, "tras sondeo fallido, shadow[0] = 128");
        CHECK_EQ_U(dev.shadow[3], AD5144_CODE_POWERUP, "tras sondeo fallido, shadow[3] = 128");
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
 *  3. AD5144_CodeForMillivolts / AD5144_SetChannelMillivolts
 * ================================================================== */

/* Modelo fisico reimplementado en el arnes, en doble precision e
 * independiente de la aritmetica entera del driver:
 *   R_AW(D) = (256 - D)/256 * R_AB + R_W
 *   |OUT|   = in_peak * R_AW(D) / R_IN                                  */
static double model_out_mv(uint8_t code, double in_peak_mv, double rab, double rw)
{
    double raw = (256.0 - (double)code) / 256.0 * rab + rw;
    return in_peak_mv * raw / (double)AD5144_RIN_OHM;
}

/* Excursion de Dif con la que trabaja la placa: 621 codigos de DAC con
 * VREF = 3,3 V -> 500 mV (DAC_DIF_PEAK_MV en dac_waveform.h). Se copia el
 * numero aqui porque el arnes no enlaza dac_waveform. */
#define BOARD_IN_PEAK_MV  (500U)

static void test_set_channel_millivolts(void)
{
    printf("[test] AD5144_CodeForMillivolts / SetChannelMillivolts: modelo de reostato inverso\n");

    AD5144_Handle_t dev;
    I2C_HandleTypeDef hi2c;
    uint8_t code;

    /* --- Validacion de argumentos --- */
    CHECK(AD5144_CodeForMillivolts(100U, 0U, &code) == HAL_ERROR, "in_peak = 0 -> HAL_ERROR");
    CHECK(AD5144_CodeForMillivolts(100U, 556U, NULL) == HAL_ERROR, "out_code NULL -> HAL_ERROR");
    init_ok(&dev, &hi2c, ADDR_U1);
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_A, 100U, 0U) == HAL_ERROR,
          "SetChannelMillivolts con in_peak = 0 -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "in_peak 0 no transmite");
    Ad5144Spy_Reset();
    CHECK(AD5144_SetChannelMillivolts(NULL, AD5144_CH_A, 100U, 556U) == HAL_ERROR,
          "dev NULL -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "dev NULL no transmite");

    /* --- Relacion INVERSA: mas amplitud -> codigo menor --- */
    uint8_t c500, c1000, c2000;
    CHECK(AD5144_CodeForMillivolts( 500U, BOARD_IN_PEAK_MV, &c500)  == HAL_OK, "500 mV alcanzable");
    CHECK(AD5144_CodeForMillivolts(1000U, BOARD_IN_PEAK_MV, &c1000) == HAL_OK, "1000 mV alcanzable");
    CHECK(AD5144_CodeForMillivolts(2000U, BOARD_IN_PEAK_MV, &c2000) == HAL_OK, "2000 mV alcanzable");
    CHECK(c500 > c1000 && c1000 > c2000, "codigo decrece al subir la amplitud");

    /* Puntos calculados a mano (R_AB 10k, R_W 55, R_IN 2k5, in_peak 500):
     *   1000 mV: R_AW = 5000,0 -> (4945,0)*256/10000 = 126,6 -> 127 -> D = 129
     *   2000 mV: R_AW = 10000  -> (9945,0)*256/10000 = 254,6 -> 255 -> D = 1 */
    CHECK_EQ_U(c1000, 129U, "1000 mV con in_peak 500 -> D = 129");
    CHECK_EQ_U(c2000, 1U,   "2000 mV con in_peak 500 -> D = 1");

    /* --- 0 mV y por debajo del minimo alcanzable -> posicion segura 255 --- */
    CHECK(AD5144_CodeForMillivolts(0U, BOARD_IN_PEAK_MV, &code) == HAL_OK, "0 mV -> HAL_OK");
    CHECK_EQ_U(code, AD5144_CODE_MIN_GAIN, "0 mV -> codigo 255 (ganancia minima)");
    CHECK(AD5144_CodeForMillivolts(5U, BOARD_IN_PEAK_MV, &code) == HAL_OK, "5 mV -> HAL_OK");
    CHECK_EQ_U(code, AD5144_CODE_MIN_GAIN, "5 mV (< minimo ~19 mV) -> codigo 255");

    /* --- Inalcanzable (mas ganancia de la que hay) -> HAL_ERROR, sin bus --- */
    init_ok(&dev, &hi2c, ADDR_U2);
    dev.shadow[AD5144_CH_B] = 0x77U;
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_B, 2000U, 100U) == HAL_ERROR,
          "2000 mV con in_peak 100 mV (ganancia 20) -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "objetivo inalcanzable: no se escribe nada");
    CHECK_EQ_U(dev.shadow[AD5144_CH_B], 0x77U, "objetivo inalcanzable: shadow intacto");

    /* Limite exacto: el maximo alcanzable con D = 0 es in_peak*(R_AB+R_W)/R_IN.
     * Con in_peak 500: 500*10055/2500 = 2011 mV. 2011 -> D = 0; 2300 -> error. */
    CHECK(AD5144_CodeForMillivolts(2011U, BOARD_IN_PEAK_MV, &code) == HAL_OK, "2011 mV justo alcanzable");
    CHECK_EQ_U(code, 0U, "2011 mV -> D = 0 (ganancia maxima)");
    CHECK(AD5144_CodeForMillivolts(2300U, BOARD_IN_PEAK_MV, &code) == HAL_ERROR, "2300 mV -> HAL_ERROR");

    /* --- Trama completa de un SetChannelMillivolts valido --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    CHECK(AD5144_SetChannelMillivolts(&dev, AD5144_CH_C, 1000U, BOARD_IN_PEAK_MV) == HAL_OK,
          "SetChannelMillivolts 1000 mV -> HAL_OK");
    expect_frame(0, ADDR_U1, 0x12U, 129U, "canal C @ 1000 mV");
    CHECK_EQ_U(dev.shadow[AD5144_CH_C], 129U, "shadow = 129");

    /* --- Barrido 0..2000 mV en pasos de 1 mV, tres excursiones de entrada.
     *     Para cada punto alcanzable por encima del minimo se comprueba,
     *     contra el modelo en doble precision, que el codigo elegido deja la
     *     salida a no mas de medio paso del objetivo. --- */
    static const uint16_t peaks[] = { BOARD_IN_PEAK_MV, 400U, 700U };
    unsigned long pts = 0UL;
    int err_ret = 0, err_round = 0, err_mono = 0;
    double worst_step = 0.0;
    for (size_t k = 0U; k < sizeof(peaks) / sizeof(peaks[0]); k++) {
        double inp   = (double)peaks[k];
        double vmin  = model_out_mv(255U, inp, AD5144_RAB_OHM, AD5144_RW_OHM);
        double vmax  = model_out_mv(0U,   inp, AD5144_RAB_OHM, AD5144_RW_OHM);
        double step  = inp * ((double)AD5144_RAB_OHM / 256.0) / (double)AD5144_RIN_OHM;
        if (step > worst_step && peaks[k] == BOARD_IN_PEAK_MV) { worst_step = step; }
        uint8_t prev = 255U;
        for (uint32_t mv = 0U; mv <= 2000U; mv++) {
            if ((double)mv > vmax + step / 2.0) { break; }
            uint8_t c;
            if (AD5144_CodeForMillivolts((uint16_t)mv, peaks[k], &c) != HAL_OK) { err_ret++; continue; }
            if (c > prev) { err_mono++; }   /* nunca sube al subir el objetivo */
            prev = c;
            if ((double)mv >= vmin) {
                double out = model_out_mv(c, inp, AD5144_RAB_OHM, AD5144_RW_OHM);
                if (fabs(out - (double)mv) > step / 2.0 + 1e-6) { err_round++; }
            }
            pts++;
        }
    }
    printf("   barrido mV->code: %lu puntos, paso con in_peak 500 mV = %.2f mV\n", pts, worst_step);
    CHECK(pts > 5000UL, "el barrido cubre miles de puntos");
    CHECK_EQ_U(err_ret, 0,   "todo objetivo <= maximo alcanzable devuelve HAL_OK");
    CHECK_EQ_U(err_mono, 0,  "el codigo es monotono decreciente con el objetivo");
    CHECK_EQ_U(err_round, 0, "error de salida <= medio paso en todo el barrido");
    CHECK(worst_step < 10.0, "resolucion con la excursion de la placa < 10 mV/paso");

    /* --- Margen de diseno con la tolerancia de R_AB (+/-8 %) y la excursion
     *     de la placa (+/-0,5 V, memoria de la etapa analogica): nominal, el
     *     codigo 0 da ~2,01 V; con R_AB -8 % sin calibrar quedan ~1,85 V, es
     *     decir, ~7,5 % por debajo de 2 V (dentro del ~15 % de error sin
     *     calibrar de la memoria); con R_AB +8 % y D = 0 la salida no supera
     *     los 2,5 V de los terminales del AD5144. --- */
    double vmax_nom  = model_out_mv(0U, BOARD_IN_PEAK_MV, AD5144_RAB_OHM, AD5144_RW_OHM);
    double vmax_low  = model_out_mv(0U, BOARD_IN_PEAK_MV, AD5144_RAB_OHM * 0.92, 55.0);
    double vmax_high = model_out_mv(0U, BOARD_IN_PEAK_MV, AD5144_RAB_OHM * 1.08, 125.0);
    printf("   salida con D = 0: %.0f mV nominal, %.0f mV (R_AB -8 %%), %.0f mV (R_AB +8 %%)\n",
           vmax_nom, vmax_low, vmax_high);
    CHECK(vmax_nom  >= 2000.0, "nominal: 2 V alcanzables con D = 0 (~2,01 V)");
    CHECK(vmax_low  >= 2000.0 * 0.85, "R_AB -8 %: a lo sumo 15 % por debajo de 2 V sin calibrar");
    CHECK(vmax_high <= 2500.0, "R_AB +8 %: D = 0 no saca los terminales de +/-2,5 V");
}

/* ================================================================== *
 *  4. AD5144_AllChannelsZero
 * ================================================================== */
static void test_all_channels_zero(void)
{
    printf("[test] AD5144_AllChannelsZero: codigo 255, orden A,B,C,D, no corta ante fallo, devuelve el ultimo fallo\n");

    AD5144_Handle_t dev;
    I2C_HandleTypeDef hi2c;

    /* --- Camino feliz: 4 escrituras de 255 (ganancia minima) en orden A,B,C,D --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    /* ensuciamos el shadow[] con escrituras previas */
    for (int i = 0; i < 4; i++) {
        AD5144_SetChannelRaw(&dev, (AD5144_Channel_t)i, (uint8_t)(0x50U + i));
    }
    Ad5144Spy_Reset();
    CHECK(AD5144_AllChannelsZero(&dev) == HAL_OK, "todo OK -> HAL_OK");
    CHECK_EQ_U(g_spy.tx_calls, 4, "exactamente 4 transacciones");
    expect_frame(0, ADDR_U1, 0x10U, 0xFFU, "AllZero paso 1 = canal A a 255");
    expect_frame(1, ADDR_U1, 0x11U, 0xFFU, "AllZero paso 2 = canal B a 255");
    expect_frame(2, ADDR_U1, 0x12U, 0xFFU, "AllZero paso 3 = canal C a 255");
    expect_frame(3, ADDR_U1, 0x13U, 0xFFU, "AllZero paso 4 = canal D a 255");
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_U(dev.shadow[i], 255U, "shadow[canal] = 255 tras AllChannelsZero");
    }
    /* Nunca escribe el codigo 0: con el reostato seria la ganancia MAXIMA. */
    for (int i = 0; i < 4; i++) {
        CHECK(g_spy.tx_b1[i] != 0x00U, "AllChannelsZero nunca escribe el codigo 0");
    }

    /* --- Un fallo a mitad NO corta el barrido y el estado devuelto es el
     *     del ULTIMO fallo (comportamiento actual; enmascara fallos
     *     intermedios -> HALLAZGO documentado en el README) --- */
    init_ok(&dev, &hi2c, ADDR_U1);
    for (int i = 0; i < 4; i++) {
        AD5144_SetChannelRaw(&dev, (AD5144_Channel_t)i, 0xEEU);   /* shadow != 255 en los 4 */
    }
    Ad5144Spy_Reset();
    g_spy.tx_ret_seq[1] = HAL_TIMEOUT;   /* canal B falla */
    g_spy.tx_ret_seq[3] = HAL_BUSY;      /* canal D falla, mas tarde */
    HAL_StatusTypeDef r = AD5144_AllChannelsZero(&dev);
    CHECK_EQ_U(g_spy.tx_calls, 4, "sigue intentando los 4 canales pese a los fallos");
    CHECK(r == HAL_BUSY, "devuelve el ultimo fallo (HAL_BUSY), no el primero (HAL_TIMEOUT)");
    CHECK_EQ_U(dev.shadow[AD5144_CH_A], 255U,  "canal A OK -> shadow 255");
    CHECK_EQ_U(dev.shadow[AD5144_CH_B], 0xEEU, "canal B fallo -> shadow intacto");
    CHECK_EQ_U(dev.shadow[AD5144_CH_C], 255U,  "canal C OK -> shadow 255");
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


/* ================================================================== *
 *  5. EEPROM: AD5144_StoreEepromCode / AD5144_StoreSafePowerUp
 * ================================================================== */
static void test_eeprom(void)
{
    printf("[test] EEPROM: Comando 11, sondeo por ACK, posicion segura\n");

    AD5144_Handle_t dev;
    I2C_HandleTypeDef hi2c;

    CHECK_EQ_U(AD5144_CMD_WRITE_EEPROM, 0x08U, "Comando 11: nibble de control 1000");
    CHECK_EQ_U(BuildCommandByte(AD5144_CMD_WRITE_EEPROM, AD5144_CH_C), 0x82U, "EEPROM canal C -> 0x82");

    /* NULL */
    Ad5144Spy_Reset();
    CHECK(AD5144_StoreEepromCode(NULL, AD5144_CH_A, 255U) == HAL_ERROR, "dev NULL -> HAL_ERROR");
    CHECK_EQ_U(g_spy.tx_calls, 0, "dev NULL no transmite");

    /* Programacion inmediata: trama y un unico sondeo */
    init_ok(&dev, &hi2c, ADDR_U2);
    dev.shadow[AD5144_CH_B] = 0x33U;
    CHECK(AD5144_StoreEepromCode(&dev, AD5144_CH_B, 0xFFU) == HAL_OK, "EEPROM canal B -> HAL_OK");
    CHECK_EQ_U(g_spy.tx_calls, 1, "una trama");
    expect_frame(0, ADDR_U2, 0x81U, 0xFFU, "EEPROM canal B @ 255");
    CHECK_EQ_U(g_spy.ready_calls, 1, "un sondeo basta si el chip ya responde");
    CHECK_EQ_U(g_spy.ready_addr, addr8(ADDR_U2), "sondeo en la direccion del chip");
    CHECK_EQ_U(dev.shadow[AD5144_CH_B], 0x33U, "escribir EEPROM no cambia el RDAC (shadow intacto)");

    /* Chip ocupado 20 ms: se reintenta cada 1 ms hasta que responde */
    init_ok(&dev, &hi2c, ADDR_U1);
    g_spy.ready_busy_left = 20;
    CHECK(AD5144_StoreEepromCode(&dev, AD5144_CH_A, 0xFFU) == HAL_OK, "ocupado 20 intentos -> HAL_OK");
    CHECK_EQ_U(g_spy.ready_calls, 21, "21 sondeos (20 NACK + 1 ACK)");
    CHECK_EQ_U(g_spy.tick_ms, 20U, "20 ms de espera, 1 ms por intento");

    /* Nunca responde: HAL_TIMEOUT a los 60 ms, sin bucle infinito */
    init_ok(&dev, &hi2c, ADDR_U1);
    g_spy.ready_busy_left = 100000;
    CHECK(AD5144_StoreEepromCode(&dev, AD5144_CH_A, 0xFFU) == HAL_TIMEOUT, "sin ACK -> HAL_TIMEOUT");
    CHECK_EQ_U(g_spy.tick_ms, AD5144_EEPROM_TIMEOUT_MS, "se rinde a los 60 ms");

    /* Fallo de la propia trama: no se sondea */
    init_ok(&dev, &hi2c, ADDR_U1);
    g_spy.tx_ret = HAL_ERROR;
    CHECK(AD5144_StoreEepromCode(&dev, AD5144_CH_A, 0xFFU) == HAL_ERROR, "trama fallida -> HAL_ERROR");
    CHECK_EQ_U(g_spy.ready_calls, 0, "sin trama no hay sondeo");

    /* StoreSafePowerUp: 255 en la EEPROM de A,B,C,D en orden */
    init_ok(&dev, &hi2c, ADDR_U1);
    CHECK(AD5144_StoreSafePowerUp(&dev) == HAL_OK, "StoreSafePowerUp -> HAL_OK");
    CHECK_EQ_U(g_spy.tx_calls, 4, "cuatro tramas");
    expect_frame(0, ADDR_U1, 0x80U, 0xFFU, "EEPROM A @ 255");
    expect_frame(1, ADDR_U1, 0x81U, 0xFFU, "EEPROM B @ 255");
    expect_frame(2, ADDR_U1, 0x82U, 0xFFU, "EEPROM C @ 255");
    expect_frame(3, ADDR_U1, 0x83U, 0xFFU, "EEPROM D @ 255");

    /* Un fallo a mitad no corta el resto */
    init_ok(&dev, &hi2c, ADDR_U1);
    g_spy.tx_ret_seq[1] = HAL_BUSY;
    CHECK(AD5144_StoreSafePowerUp(&dev) == HAL_BUSY, "fallo en B -> devuelve HAL_BUSY");
    CHECK_EQ_U(g_spy.tx_calls, 4, "sigue con C y D");
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
    test_eeprom();

    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
