/**
 ******************************************************************************
 * @file    culture_selector.c
 * @brief   Selector de pocillo (ADG1634, U3/U4) -- interruptor simple.
 ******************************************************************************
 */
#include "culture_selector.h"

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} WellPin_t;

/* Orden: pocillo 1..8. Ajustar si la asignacion final en Altium cambia. */
static const WellPin_t kWellPins[8] = {
    { GPIOE, GPIO_PIN_9  }, /* pocillo 1 */
    { GPIOE, GPIO_PIN_11 }, /* pocillo 2 */
    { GPIOE, GPIO_PIN_13 }, /* pocillo 3 */
    { GPIOE, GPIO_PIN_14 }, /* pocillo 4 */
    { GPIOD, GPIO_PIN_12 }, /* pocillo 5 */
    { GPIOD, GPIO_PIN_13 }, /* pocillo 6 */
    { GPIOD, GPIO_PIN_14 }, /* pocillo 7 */
    { GPIOD, GPIO_PIN_15 }, /* pocillo 8 */
};

HAL_StatusTypeDef CultureSelector_SetWell(uint8_t well, bool connected)
{
    if (well < CULTURE_WELL_MIN || well > CULTURE_WELL_MAX) {
        return HAL_ERROR;
    }

    const WellPin_t *wp = &kWellPins[well - 1U];
    HAL_GPIO_WritePin(wp->port, wp->pin, connected ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return HAL_OK;
}

HAL_StatusTypeDef CultureSelector_AllDisconnect(void)
{
    HAL_StatusTypeDef status = HAL_OK;
    for (uint8_t well = CULTURE_WELL_MIN; well <= CULTURE_WELL_MAX; well++) {
        HAL_StatusTypeDef s = CultureSelector_SetWell(well, false);
        if (s != HAL_OK) {
            status = s;
        }
    }
    return status;
}
