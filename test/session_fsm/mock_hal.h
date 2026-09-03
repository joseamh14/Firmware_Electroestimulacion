/**
 ******************************************************************************
 * @file    mock_hal.h
 * @brief   Sustituto minimo de "stm32l4xx_hal.h" para el arnes de pruebas de
 *          escritorio de session_fsm.
 *
 * Solo declara la superficie de HAL que tocan session_fsm.c y las cabeceras
 * de BSP que este arrastra (ad5144.h / dac_waveform.h / culture_selector.h):
 * tipos de handle, calendario del RTC y registros de backup. La logica de la
 * FSM se compila SIN modificar contra este mock; las implementaciones estan
 * en mocks.c.
 ******************************************************************************
 */
#ifndef TEST_MOCK_HAL_H_
#define TEST_MOCK_HAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --- Estado de retorno de HAL ------------------------------------------- */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03
} HAL_StatusTypeDef;

/* --- Formatos del RTC -------------------------------------------------- */
#define RTC_FORMAT_BIN   (0U)
#define RTC_FORMAT_BCD   (1U)

/* --- Registros de backup del RTC (valor == indice, igual que en HAL) --- */
#define RTC_BKP_DR0   (0U)
#define RTC_BKP_DR1   (1U)
#define RTC_BKP_DR2   (2U)
#define RTC_BKP_DR3   (3U)
#define RTC_BKP_DR4   (4U)
#define RTC_BKP_DR5   (5U)
#define RTC_BKP_DR6   (6U)
#define RTC_BKP_DR7   (7U)
#define RTC_BKP_DR8   (8U)
#define RTC_BKP_DR9   (9U)
#define RTC_BKP_DR10  (10U)
#define RTC_BKP_DR11  (11U)
#define RTC_BKP_DR12  (12U)
#define RTC_BKP_DR13  (13U)
#define RTC_BKP_DR14  (14U)
#define RTC_BKP_DR31  (31U)
#define MOCK_BKP_COUNT (32U)

/* --- Handles (opacos para la FSM) ------------------------------------- */
typedef struct { uint32_t dummy; } I2C_HandleTypeDef;
typedef struct { uint32_t dummy; } DAC_HandleTypeDef;
typedef struct { uint32_t dummy; } TIM_HandleTypeDef;

typedef struct { uint32_t Instance; } RTC_HandleTypeDef;

/* --- Calendario del RTC (formato BIN) -------------------------------- */
typedef struct {
    uint8_t Hours;
    uint8_t Minutes;
    uint8_t Seconds;
    uint8_t DayLightSaving;
    uint8_t StoreOperation;
    uint32_t SubSeconds;
} RTC_TimeTypeDef;

typedef struct {
    uint8_t WeekDay;
    uint8_t Month;
    uint8_t Date;
    uint8_t Year;
} RTC_DateTypeDef;

/* --- API de HAL que usa session_fsm.c -------------------------------- */
void              HAL_PWR_EnableBkUpAccess(void);
HAL_StatusTypeDef HAL_RTC_GetTime(RTC_HandleTypeDef *hrtc, RTC_TimeTypeDef *sTime, uint32_t Format);
HAL_StatusTypeDef HAL_RTC_GetDate(RTC_HandleTypeDef *hrtc, RTC_DateTypeDef *sDate, uint32_t Format);
uint32_t          HAL_RTCEx_BKUPRead(RTC_HandleTypeDef *hrtc, uint32_t BackupRegister);
void              HAL_RTCEx_BKUPWrite(RTC_HandleTypeDef *hrtc, uint32_t BackupRegister, uint32_t Data);

/* ===================================================================== *
 *  Controles del banco de pruebas (no forman parte de HAL)
 * ===================================================================== */

/* Reloj falso del RTC: segundos "unix" (origen 1970-01-01). La FSM solo
 * mira diferencias, asi que el origen exacto es indiferente. */
void     MockRTC_SetUnix(uint64_t seconds);
void     MockRTC_AdvanceSeconds(uint32_t seconds);
uint64_t MockRTC_GetUnix(void);

/* Registros de backup con respaldo tipo VBAT (persisten entre "reinicios",
 * es decir, entre llamadas sucesivas a SessionFSM_Init). */
void      MockBKP_Reset(void);
uint32_t *MockBKP_Raw(void);

/* Espias de las llamadas que la FSM hace a la capa BSP. */
typedef struct {
    int      dac_setfreq_calls;
    uint32_t dac_setfreq_last[2];
    int      dac_stop_calls[2];
    int      ad_allzero_calls;
    int      ad_setmv_calls;
    uint16_t ad_setmv_last_mv[8];   /* indexado por canal absoluto 0..7   */
    int      sel_setwell_calls;
    int      sel_alldisconnect_calls;
    uint8_t  sel_well_state;        /* mascara reconstruida de SetWell     */
} MockSpy_t;

extern MockSpy_t g_spy;
void MockSpy_Reset(void);

#endif /* TEST_MOCK_HAL_H_ */
