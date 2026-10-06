/**
 ******************************************************************************
 * @file    current_log.c
 * @brief   Valor por canal de cada ventana y formato del registro de 36 B.
 ******************************************************************************
 */
#include "current_log.h"
#include "current_calc.h"
#include <string.h>

float CurrentLog_NaN(void)
{
    const uint32_t bits = 0x7FC00000UL;
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

void CurrentLog_Compute(const TiaWindow_t *win, float out_ua[CURRENT_LOG_NUM_CH])
{
    for (uint8_t i = 0U; i < CURRENT_LOG_NUM_CH; i++) {
        out_ua[i] = CurrentLog_NaN();
        if (win == NULL || win->count[i] == 0U) {
            continue;
        }
        float n = (float)win->count[i];
        float code_max = (float)win->sum_max[i] / n;
        float code_min = (float)win->sum_min[i] / n;
        float i_pos;
        float i_neg;
        if (CurrentCalc_CodeFToMicroamps((uint8_t)(i + 1U), code_max, &i_pos) != HAL_OK ||
            CurrentCalc_CodeFToMicroamps((uint8_t)(i + 1U), code_min, &i_neg) != HAL_OK) {
            continue;
        }
        out_ua[i] = (i_pos - i_neg) * 0.5f;
    }
}

static void PutU32LE(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

void CurrentLog_Pack(uint32_t ms, const float val_ua[CURRENT_LOG_NUM_CH],
                     uint8_t out[CURRENT_LOG_RECORD_BYTES])
{
    PutU32LE(&out[0], ms);
    for (uint8_t i = 0U; i < CURRENT_LOG_NUM_CH; i++) {
        uint32_t bits;
        memcpy(&bits, &val_ua[i], sizeof(bits));
        PutU32LE(&out[4U + 4U * i], bits);
    }
}
