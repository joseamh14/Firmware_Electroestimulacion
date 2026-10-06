/**
 ******************************************************************************
 * @file    test_flash_log.c
 * @brief   Arnes de pruebas de escritorio para BSP/Src/flash_log.c.
 *
 * Compila el modulo real con FLASH_LOG_HOST_TEST contra el simulador de
 * flash de mocks.c. Cubre:
 *   1. CRC-32 contra el vector de comprobacion estandar ("123456789").
 *   2. Validacion de la configuracion y de la region reservada.
 *   3. Escritura y lectura del ultimo registro, secuencia creciente.
 *   4. Rotacion entre los dos sectores: cuando y cuanto se borra, y que el
 *      sector con el ultimo registro valido nunca se borra antes de escribir
 *      uno nuevo.
 *   5. Corte de alimentacion en CADA operacion de flash de una escritura
 *      (incluido el borrado de sector): tras el reinicio el ultimo registro
 *      legible es el anterior o el nuevo, nunca basura, y el registro sigue
 *      siendo utilizable.
 *   6. Registros con error ECC doble (NMI simulada) se descartan.
 *   7. Desbordamiento del numero de secuencia.
 *   8. Dos registros independientes (sesion y calibracion) no se pisan.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "flash_log.h"

#include <stdio.h>
#include <string.h>

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

/* Payload de prueba de 52 bytes, como el de la sesion (registro de 64 B). */
typedef struct { uint32_t w[13]; } Pay_t;

static const FlashLog_Config_t kSes = {
    { FLASH_LOG_SESSION_SECTOR_A, FLASH_LOG_SESSION_SECTOR_B }, 0x53455346UL,
    FLASH_LOG_RECORD_SIZE_FOR(sizeof(Pay_t))
};
static const FlashLog_Config_t kCal = {
    { FLASH_LOG_CAL_SECTOR_A, FLASH_LOG_CAL_SECTOR_B }, 0x43414C31UL,
    FLASH_LOG_RECORD_SIZE_FOR(68U)
};

static Pay_t make_pay(uint32_t tag)
{
    Pay_t p;
    for (int i = 0; i < 13; i++) {
        p.w[i] = tag * 1000U + (uint32_t)i;
    }
    return p;
}

static uint32_t tag_of(const Pay_t *p) { return p->w[0] / 1000U; }

static bool pay_is_tag(const Pay_t *p, uint32_t tag)
{
    Pay_t ref = make_pay(tag);
    return memcmp(p, &ref, sizeof(ref)) == 0;
}

#define SLOTS_PER_SECTOR  (FLASH_LOG_SECTOR_SIZE / 64U)   /* 128 */

/* ================================================================== */
static void test_crc(void)
{
    printf("[test] CRC-32 (vector estandar)\n");
    const uint8_t v[] = "123456789";
    CHECK_EQ_U(FlashLog_Crc32(v, 9U), 0xCBF43926UL, "CRC-32(\"123456789\") = 0xCBF43926");
    CHECK_EQ_U(FlashLog_Crc32(v, 0U), 0x00000000UL, "CRC-32 de 0 bytes = 0");
}

static void test_config(void)
{
    printf("[test] validacion de configuracion y region\n");
    SimFlash_Reset();
    Pay_t p = make_pay(1);
    FlashLog_Config_t c;

    CHECK_EQ_U(kSes.record_size, 64U, "payload 52 B -> registro de 64 B");
    CHECK_EQ_U(kCal.record_size, 80U, "payload 68 B -> registro de 80 B");

    CHECK(FlashLog_Append(NULL, &p, sizeof(p)) == HAL_ERROR, "cfg NULL");
    CHECK(FlashLog_Append(&kSes, NULL, sizeof(p)) == HAL_ERROR, "payload NULL");
    CHECK(FlashLog_ReadLatest(&kSes, NULL, sizeof(p), NULL) == HAL_ERROR, "destino NULL");

    c = kSes; c.record_size = 72U;
    CHECK(FlashLog_Append(&c, &p, sizeof(p)) == HAL_ERROR, "registro no multiplo de 16");
    c = kSes; c.record_size = 272U;
    CHECK(FlashLog_Append(&c, &p, sizeof(p)) == HAL_ERROR, "registro > 256 B");
    c = kSes; c.record_size = 48U;
    CHECK(FlashLog_Append(&c, &p, sizeof(p)) == HAL_ERROR, "payload no cabe en el registro");
    c = kSes; c.magic = 0xFFFFFFFFUL;
    CHECK(FlashLog_Append(&c, &p, sizeof(p)) == HAL_ERROR, "magic igual a flash borrada");
    CHECK_EQ_U(SimFlash_ProgramCount(), 0, "ningun intento invalido programa la flash");

    SimFlash_SetImageEnd(FLASH_LOG_REGION_START + 8U);
    CHECK(!FlashLog_RegionIsFree(), "imagen que invade la region -> no libre");
    CHECK(FlashLog_Append(&kSes, &p, sizeof(p)) == HAL_ERROR, "sin region libre no se escribe");
    Pay_t r;
    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), NULL) == HAL_ERROR, "sin region libre no se lee");
    SimFlash_SetImageEnd(FLASH_LOG_REGION_START);
    CHECK(FlashLog_RegionIsFree(), "imagen que acaba justo en la region -> libre");
}

static void test_basic(void)
{
    printf("[test] escritura / lectura / secuencia\n");
    SimFlash_Reset();
    Pay_t r;
    uint32_t seq = 0U;

    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), &seq) == HAL_ERROR, "flash vacia -> sin registro");

    for (uint32_t t = 1U; t <= 10U; t++) {
        Pay_t p = make_pay(t);
        CHECK(FlashLog_Append(&kSes, &p, sizeof(p)) == HAL_OK, "append OK");
        CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), &seq) == HAL_OK, "lectura OK");
        CHECK(pay_is_tag(&r, t), "el ultimo leido es el ultimo escrito");
        CHECK_EQ_U(seq, t, "secuencia = numero de escrituras");
    }
    CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_SESSION_SECTOR_A), 0, "sin borrados mientras cabe");
    CHECK_EQ_U(SimFlash_ProgramCount(), 10 * 8, "8 dobles palabras por registro de 64 B");

    /* Payload parcial: se puede leer menos bytes de los guardados */
    uint32_t first;
    CHECK(FlashLog_ReadLatest(&kSes, &first, 4U, NULL) == HAL_OK, "lectura de 4 bytes");
    CHECK_EQ_U(first, 10U * 1000U, "primer campo del ultimo registro");
}

static void test_rotation(void)
{
    printf("[test] rotacion entre sectores\n");
    SimFlash_Reset();
    Pay_t r;
    uint32_t seq;
    int errors = 0;

    for (uint32_t t = 1U; t <= 4U * SLOTS_PER_SECTOR + 7U; t++) {
        Pay_t p = make_pay(t);
        if (FlashLog_Append(&kSes, &p, sizeof(p)) != HAL_OK) { errors++; }
        if (FlashLog_ReadLatest(&kSes, &r, sizeof(r), &seq) != HAL_OK || !pay_is_tag(&r, t) || seq != t) {
            errors++;
        }
        if (t == SLOTS_PER_SECTOR) {
            CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_SESSION_SECTOR_B), 0, "sector A lleno: B aun sin borrar");
        }
        if (t == SLOTS_PER_SECTOR + 1U) {
            CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_SESSION_SECTOR_B), 1, "registro 129: se borra B");
            CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_SESSION_SECTOR_A), 0, "A intacto al pasar a B");
        }
        if (t == 2U * SLOTS_PER_SECTOR + 1U) {
            CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_SESSION_SECTOR_A), 1, "registro 257: se borra A");
        }
    }
    CHECK_EQ_U(errors, 0, "519 escrituras: cada lectura devuelve la ultima");
    CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_SESSION_SECTOR_A), 2, "A borrado 2 veces");
    CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_SESSION_SECTOR_B), 2, "B borrado 2 veces");
}

/* Corte de alimentacion en la operacion k de una escritura. */
static void test_power_cut(void)
{
    printf("[test] corte de alimentacion en cada operacion de una escritura\n");
    static const uint32_t prefill[] = { 0U, 1U, 5U, SLOTS_PER_SECTOR - 1U, SLOTS_PER_SECTOR,
                                        2U * SLOTS_PER_SECTOR, 2U * SLOTS_PER_SECTOR + 3U };
    int cases = 0, bad_read = 0, bad_after = 0, garbage = 0;

    for (int mode = 0; mode < 2; mode++) {
    for (size_t i = 0U; i < sizeof(prefill) / sizeof(prefill[0]); i++) {
        for (int k = 0; k <= 9; k++) {   /* hasta 1 borrado + 8 programaciones */
            SimFlash_Reset();
            SimFlash_CutClean(mode == 1);
            for (uint32_t t = 1U; t <= prefill[i]; t++) {
                Pay_t p = make_pay(t);
                (void)FlashLog_Append(&kSes, &p, sizeof(p));
            }
            uint32_t newtag = prefill[i] + 1U;
            Pay_t pn = make_pay(newtag);
            SimFlash_PowerCutAfter(k);
            HAL_StatusTypeDef st = FlashLog_Append(&kSes, &pn, sizeof(pn));
            bool was_cut = SimFlash_IsDown();
            SimFlash_Reboot();
            cases++;

            Pay_t r;
            HAL_StatusTypeDef rd = FlashLog_ReadLatest(&kSes, &r, sizeof(r), NULL);
            if (!was_cut) {
                /* la escritura termino antes de la operacion k */
                if (st != HAL_OK || rd != HAL_OK || !pay_is_tag(&r, newtag)) { bad_read++; }
            } else if (prefill[i] == 0U) {
                /* no habia nada: o nada, o el nuevo si llego a completarse */
                if (rd == HAL_OK && !pay_is_tag(&r, newtag)) { garbage++; }
            } else {
                if (rd != HAL_OK) { bad_read++; }
                else if (!pay_is_tag(&r, prefill[i]) && !pay_is_tag(&r, newtag)) { garbage++; }
            }

            /* Tras el reinicio el registro sigue siendo utilizable */
            Pay_t pa = make_pay(9999U);
            if (FlashLog_Append(&kSes, &pa, sizeof(pa)) != HAL_OK ||
                FlashLog_ReadLatest(&kSes, &r, sizeof(r), NULL) != HAL_OK ||
                !pay_is_tag(&r, 9999U)) {
                bad_after++;
            }
        }
    }
    }
    printf("   %d escenarios de corte (con y sin error ECC en la doble palabra cortada)\n", cases);
    CHECK_EQ_U(garbage, 0, "nunca se lee un registro a medias");
    CHECK_EQ_U(bad_read, 0, "con registro previo, siempre se recupera el anterior o el nuevo");
    CHECK_EQ_U(bad_after, 0, "tras el corte se puede seguir escribiendo y leyendo");
}

static void test_ecc(void)
{
    printf("[test] registros con error ECC doble\n");
    SimFlash_Reset();
    for (uint32_t t = 1U; t <= 5U; t++) {
        Pay_t p = make_pay(t);
        (void)FlashLog_Append(&kSes, &p, sizeof(p));
    }
    /* El registro 5 (el ultimo) queda con una doble palabra ilegible */
    SimFlash_CorruptDw(FLASH_LOG_SESSION_SECTOR_A + 4U * 64U + 24U);
    Pay_t r;
    uint32_t seq;
    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), &seq) == HAL_OK, "hay registro valido");
    CHECK(pay_is_tag(&r, 4U), "el registro con ECC se descarta: gana el 4");
    CHECK_EQ_U(seq, 4U, "secuencia 4");
    CHECK(!FlashLog_EccFault, "el indicador de ECC queda limpio tras la lectura");

    Pay_t p6 = make_pay(6U);
    CHECK(FlashLog_Append(&kSes, &p6, sizeof(p6)) == HAL_OK, "se puede seguir escribiendo");
    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), &seq) == HAL_OK && pay_is_tag(&r, 6U), "lee el nuevo");
    CHECK_EQ_U(seq, 5U, "secuencia sigue a la ultima valida");

    /* Primera doble palabra de un hueco corrupta: el hueco cuenta como usado */
    SimFlash_CorruptDw(FLASH_LOG_SESSION_SECTOR_A + 6U * 64U);
    Pay_t p7 = make_pay(7U);
    CHECK(FlashLog_Append(&kSes, &p7, sizeof(p7)) == HAL_OK, "escribe saltando el hueco corrupto");
    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), NULL) == HAL_OK && pay_is_tag(&r, 7U), "lee el 7");
}

/* Fabrica un registro valido a mano con la secuencia dada. */
static void raw_record(uint32_t addr, uint32_t seq, uint32_t tag)
{
    uint8_t buf[64];
    Pay_t p = make_pay(tag);
    memset(buf, 0, sizeof(buf));
    uint32_t magic = kSes.magic;
    memcpy(&buf[0], &magic, 4);
    memcpy(&buf[4], &seq, 4);
    memcpy(&buf[8], &p, sizeof(p));
    uint32_t crc = FlashLog_Crc32(buf, 60U);
    memcpy(&buf[60], &crc, 4);
    memcpy(SimFlash_Raw(addr), buf, sizeof(buf));
}

static void test_seq_wrap(void)
{
    printf("[test] desbordamiento de la secuencia\n");
    SimFlash_Reset();
    raw_record(FLASH_LOG_SESSION_SECTOR_A, 0xFFFFFFFEUL, 1U);
    Pay_t r;
    uint32_t seq;
    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), &seq) == HAL_OK && seq == 0xFFFFFFFEUL, "parte de 0xFFFFFFFE");
    Pay_t p2 = make_pay(2U), p3 = make_pay(3U);
    (void)FlashLog_Append(&kSes, &p2, sizeof(p2));
    (void)FlashLog_Append(&kSes, &p3, sizeof(p3));
    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), &seq) == HAL_OK, "lectura tras desbordar");
    CHECK_EQ_U(seq, 0U, "0xFFFFFFFE -> 0xFFFFFFFF -> 0");
    CHECK(pay_is_tag(&r, 3U), "gana el registro con secuencia 0 (el mas nuevo)");
}

static void test_two_logs(void)
{
    printf("[test] registros de sesion y calibracion independientes\n");
    SimFlash_Reset();
    uint8_t cal[68], calr[68];
    for (int i = 0; i < 68; i++) { cal[i] = (uint8_t)(i * 3); }
    for (uint32_t t = 1U; t <= 300U; t++) {
        Pay_t p = make_pay(t);
        (void)FlashLog_Append(&kSes, &p, sizeof(p));
        if ((t % 100U) == 0U) {
            cal[0] = (uint8_t)t;
            (void)FlashLog_Append(&kCal, cal, sizeof(cal));
        }
    }
    Pay_t r;
    CHECK(FlashLog_ReadLatest(&kSes, &r, sizeof(r), NULL) == HAL_OK && tag_of(&r) == 300U, "sesion: ultimo = 300");
    CHECK(FlashLog_ReadLatest(&kCal, calr, sizeof(calr), NULL) == HAL_OK, "calibracion legible");
    CHECK(memcmp(cal, calr, sizeof(cal)) == 0, "calibracion: ultimo = el de t=300");
    CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_CAL_SECTOR_A), 0, "la sesion no borra sectores de calibracion");
    CHECK_EQ_U(SimFlash_EraseCount(FLASH_LOG_CAL_SECTOR_B), 0, "idem sector B de calibracion");

    /* Un log no confunde los registros del otro aunque compartieran sector */
    FlashLog_Config_t otro = kCal;
    otro.sector[0] = FLASH_LOG_SESSION_SECTOR_A;
    otro.sector[1] = FLASH_LOG_SESSION_SECTOR_B;
    CHECK(FlashLog_ReadLatest(&otro, calr, sizeof(calr), NULL) == HAL_ERROR,
          "otra marca en los mismos sectores -> ningun registro valido");
}

int main(void)
{
    printf("=== Arnes de pruebas de flash_log ===\n");
    test_crc();
    test_config();
    test_basic();
    test_rotation();
    test_power_cut();
    test_ecc();
    test_seq_wrap();
    test_two_logs();
    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
