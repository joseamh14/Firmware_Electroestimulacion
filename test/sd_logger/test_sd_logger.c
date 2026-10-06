/**
 ******************************************************************************
 * @file    test_sd_logger.c
 * @brief   Arnes de pruebas de escritorio para App/Src/sd_logger.c.
 *
 * FatFs simulado en RAM (fatfs.h / mocks.c). Cubre:
 *   1. Un fichero nuevo por tramo STIM_ON (primer nombre libre BNNNN.BIN),
 *      cerrado al pasar a REST; en REST no se mide ni se escribe.
 *   2. Un registro de 36 bytes por segundo con la marca en ms desde el
 *      inicio del tramo y los 8 valores; f_sync cada 10 registros.
 *   3. La lectura de corriente esta en pausa durante cada escritura.
 *   4. Sin tarjeta: se sigue midiendo (valores disponibles) sin registrar;
 *      se reintenta en el siguiente tramo.
 *   5. Error de escritura a mitad: se cierra el fichero y se sigue midiendo.
 *   6. Bucle principal muy retrasado: no se acumulan ventanas atrasadas.
 ******************************************************************************
 */
#include "mock_hal.h"
#include "fatfs.h"
#include "test_spy.h"
#include "sd_logger.h"
#include "current_calc.h"

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

static void boot(void)
{
    SimFs_Reset();
    memset(&g_spy, 0, sizeof(g_spy));
    g_spy.tick = 5000U;
    g_spy.state = SESSION_STATE_IDLE;
    CurrentCalc_Init();
    SdLogger_Init();
}

/* Avanza 'ms' milisegundos llamando a la tarea cada ms. */
static void run_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        g_spy.tick++;
        SdLogger_Task();
    }
}

static uint32_t rec_ms(int file, int k)
{
    const uint8_t *p = &g_fs.data[file][36 * k];
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float rec_val(int file, int k, int ch)
{
    float f;
    memcpy(&f, &g_fs.data[file][36 * k + 4 + 4 * ch], 4);
    return f;
}

static void test_blocks(void)
{
    printf("[test] un fichero por tramo, registro por segundo\n");
    boot();
    run_ms(3000);
    CHECK_EQ_U(g_spy.starts, 0, "en IDLE no se mide");
    CHECK_EQ_U(g_fs.opens, 0, "en IDLE no se abre fichero");

    /* canal 3 con picos 2048 +/- 1266 en todas las ventanas */
    g_spy.win.count[2] = 10;
    g_spy.win.sum_max[2] = 10U * (2048U + 1266U);
    g_spy.win.sum_min[2] = 10U * (2048U - 1266U);

    g_spy.state = SESSION_STATE_STIM_ON;
    run_ms(1);
    CHECK_EQ_U(g_spy.starts, 1, "STIM_ON arranca la lectura");
    CHECK_EQ_U(g_spy.mask, 0xFF, "los 8 canales");
    CHECK_EQ_U(g_fs.opens, 1, "abre un fichero");
    CHECK(strcmp(g_fs.name[0], "B0001.BIN") == 0, "primer nombre libre: B0001.BIN");

    run_ms(25000);   /* 25 s */
    CHECK_EQ_U(g_fs.size[0], 25U * 36U, "25 registros de 36 bytes");
    CHECK_EQ_U(g_fs.syncs, 2, "f_sync cada 10 registros");
    CHECK_EQ_U(rec_ms(0, 0), 1000, "primer registro a los 1000 ms");
    CHECK_EQ_U(rec_ms(0, 24), 25000, "registro 25 a los 25000 ms");
    float expect = 1266.0f * 3300.0f / 4096.0f / 1020.0f * 1000.0f;
    CHECK(rec_val(0, 3, 2) > expect - 0.01f && rec_val(0, 3, 2) < expect + 0.01f, "canal 3: ~1000 uA");
    CHECK(rec_val(0, 3, 0) != rec_val(0, 3, 0), "canal sin medidas: NaN");
    CHECK_EQ_U(g_spy.pauses, 25, "una pausa por escritura");
    CHECK_EQ_U(g_spy.write_while_paused, 25, "toda escritura ocurre con la lectura en pausa");
    CHECK(!g_spy.paused, "la lectura se reanuda tras escribir");

    SdLogStatus_t st;
    SdLogger_GetStatus(&st);
    CHECK(st.state == SD_LOG_ACTIVE && st.records == 25 && strcmp(st.file, "B0001.BIN") == 0, "estado: ACTIVE, 25, B0001");

    g_spy.state = SESSION_STATE_STIM_REST;
    run_ms(1);
    CHECK_EQ_U(g_spy.stops, 1, "REST para la lectura");
    CHECK_EQ_U(g_fs.closes, 1, "REST cierra el fichero");
    uint32_t size = g_fs.size[0];
    run_ms(5000);
    CHECK_EQ_U(g_fs.size[0], size, "en REST no se escribe");
    CHECK_EQ_U(g_fs.synced[0], size, "lo escrito queda volcado al cerrar");

    g_spy.state = SESSION_STATE_STIM_ON;
    run_ms(1);
    CHECK(strcmp(g_fs.name[1], "B0002.BIN") == 0, "tramo siguiente: B0002.BIN");
    run_ms(3000);
    CHECK_EQ_U(rec_ms(1, 0), 1000, "la marca de tiempo empieza de nuevo en cada tramo");
    g_spy.state = SESSION_STATE_IDLE;
    run_ms(1);
    CHECK_EQ_U(g_fs.closes, 2, "IDLE cierra el fichero");
}

static void test_no_card(void)
{
    printf("[test] sin tarjeta y error de escritura\n");
    boot();
    g_fs.card_present = 0;
    g_spy.win.count[0] = 1; g_spy.win.sum_max[0] = 3000; g_spy.win.sum_min[0] = 1000;
    g_spy.state = SESSION_STATE_STIM_ON;
    run_ms(3001);
    SdLogStatus_t st;
    SdLogger_GetStatus(&st);
    CHECK(st.state == SD_LOG_NO_CARD, "sin tarjeta: NOSD");
    CHECK_EQ_U(g_spy.starts, 1, "pero se mide");
    CHECK(st.have_values && st.last_ua[0] == st.last_ua[0], "valores del ultimo segundo disponibles");
    CHECK_EQ_U(g_fs.writes, 0, "nada escrito");

    g_spy.state = SESSION_STATE_STIM_REST; run_ms(10);
    g_fs.card_present = 1;
    g_spy.state = SESSION_STATE_STIM_ON; run_ms(2001);
    SdLogger_GetStatus(&st);
    CHECK(st.state == SD_LOG_ACTIVE, "tarjeta insertada: se registra en el tramo siguiente");
    CHECK_EQ_U(st.records, 2, "2 registros");

    /* error de escritura a mitad */
    g_fs.fail_write_after = 1;
    run_ms(3000);
    SdLogger_GetStatus(&st);
    CHECK(st.state == SD_LOG_NO_CARD, "error de escritura: NOSD");
    CHECK(g_fs.open_idx == -1, "fichero cerrado tras el error");
    CHECK(g_spy.running, "la lectura sigue en marcha");
    CHECK(!g_spy.paused, "y no queda en pausa");
}

static void test_late_loop(void)
{
    printf("[test] bucle principal retrasado\n");
    boot();
    g_spy.state = SESSION_STATE_STIM_ON;
    run_ms(1);
    g_spy.tick += 5500U;          /* el bucle se queda 5,5 s sin pasar */
    SdLogger_Task();
    SdLogger_Task();
    CHECK_EQ_U(g_spy.takes, 1, "una sola ventana al recuperarse, no 5");
    run_ms(999);
    CHECK_EQ_U(g_spy.takes, 1, "la siguiente, un segundo despues");
    run_ms(1);
    CHECK_EQ_U(g_spy.takes, 2, "a los 1000 ms");
}

int main(void)
{
    printf("=== Arnes de pruebas de sd_logger ===\n");
    test_blocks();
    test_no_card();
    test_late_loop();
    printf("\n=== %d comprobaciones, %d fallos ===\n", g_checks, g_fail);
    if (g_fail == 0) { printf("RESULTADO: OK\n"); return 0; }
    printf("RESULTADO: FALLO\n");
    return 1;
}
