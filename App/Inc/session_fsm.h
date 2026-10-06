/**
 ******************************************************************************
 * @file    session_fsm.h
 * @brief   Maquina de estados del protocolo autonomo de estimulacion
 *          (ciclo ON / REST de larga duracion, tipico 6 h / 6 h).
 *
 * La sesion la arma el host (ver uart_protocol.h) con START <ON> <OFF> <dias>.
 * A partir de ahi la FSM alterna sola entre estimulacion y reposo hasta que
 * se cumple la duracion total o el host manda STOP:
 *
 *   IDLE       -> todo a 0 V, DAC parado, pocillos desconectados. Estado de
 *                 arranque y destino de STOP / fin de sesion / cualquier
 *                 condicion anomala.
 *   STIM_ON    -> ambos bancos oscilando a su frecuencia configurada, cada
 *                 canal a su amplitud configurada, pocillos segun mascara.
 *   STIM_REST  -> frecuencia a 0 y amplitudes a 0 (mismo apagado seguro que
 *                 IDLE), pero el reloj de la sesion sigue contando: al
 *                 terminar el tramo de reposo se vuelve a STIM_ON con las
 *                 mismas consignas.
 *
 * PERSISTENCIA ANTE CORTE DE ALIMENTACION
 * --------------------------------------
 * Los parametros de la sesion, sus consignas y el instante de arranque se
 * guardan en los registros de backup del RTC (RTC->BKPxR), que sobreviven
 * a un reset mientras VBAT este alimentado. Al arrancar, SessionFSM_Init()
 * relee ese bloque; si es coherente, RECALCULA en que punto del ciclo
 * ON/REST deberia estar AHORA (a partir del calendario del RTC) y reanuda
 * ahi -- se autocorrige aunque el corte haya durado varios tramos.
 *
 * COPIA EN FLASH (revision de esquematicos de octubre de 2026): en esta
 * placa VBAT esta unido a VDD, asi que un corte de alimentacion borra los
 * registros de backup y reinicia el calendario. La sesion se guarda tambien
 * en la flash interna (flash_log.h) con el tiempo de sesion transcurrido y
 * un punto de control por minuto. Si al arrancar el backup del RTC no es
 * valido, se reanuda desde esa copia: se pierde como mucho un minuto y el
 * tiempo sin alimentacion no cuenta. El backup del RTC sigue siendo la
 * fuente preferente (reset por IWDG, o VBAT con pila si se anade).
 *
 * REQUISITO DE INTEGRACION: MX_RTC_Init() genera por defecto una escritura
 * incondicional de hora/fecha en cada arranque, lo que reiniciaria el
 * calendario y romperia el calculo de tiempo transcurrido. Esa escritura
 * debe quedar protegida por el patron estandar de CubeMX (bloque
 * USER CODE Check_RTC_BKUP), de forma que solo se ejecute la primera vez.
 * La FSM solo necesita que el calendario del RTC SIGA CONTANDO entre
 * arranques; su valor absoluto de "hora real" es indiferente.
 *
 * SEGURIDAD (ver capitulo de Requisitos de diseno de la memoria)
 * -------------------------------------------------------------
 *  - Arranque siempre en 0 V hasta que el host arme una sesion.
 *  - Toda consigna fuera de rango se rechaza entera (HAL_ERROR), nunca se
 *    recorta ni se aplica a medias.
 *  - Ante backup corrupto o reloj incoherente -> IDLE (0 V), no se estimula
 *    a ciegas.
 ******************************************************************************
 */
#ifndef APP_INC_SESSION_FSM_H_
#define APP_INC_SESSION_FSM_H_

#include "stm32l4xx_hal.h"
#include "ad5144.h"
#include "dac_waveform.h"
#include "culture_selector.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Numero de canales de estimulacion (2 bancos de 4). */
#define SESSION_NUM_CHANNELS            (8U)

/* Limites de amplitud por canal. El hardware permite hasta 2 V aunque el
 * uso habitual del investigador llegue a ~1 V: el firmware no restringe el
 * instrumento por debajo de lo que el hardware da. Paso de 10 mV (0,01 V). */
#define SESSION_AMP_MIN_MV              (0U)
#define SESSION_AMP_MAX_MV              (2000U)
#define SESSION_AMP_STEP_MV             (10U)

/* Amplitud de pico de la senal diferencial Dif que recibe cada canal a la
 * entrada del inversor de ESA. La fija la excursion del DAC del banco
 * (DAC_DIF_PEAK_MV, ver dac_waveform.h) y es la referencia con la que el
 * AD5144 convierte la consigna en mV a codigo de wiper (ver ad5144.h). */
#define SESSION_CHANNEL_IN_PEAK_MV     (DAC_DIF_PEAK_MV)

/* Limites del comando START. horas_on y dias son obligatorios (>=1);
 * horas_off puede ser 0 (estimulacion continua sin tramos de reposo). */
#define SESSION_ON_HOURS_MIN           (1UL)
#define SESSION_ON_HOURS_MAX           (168UL)
#define SESSION_OFF_HOURS_MIN          (0UL)
#define SESSION_OFF_HOURS_MAX          (168UL)
#define SESSION_DAYS_MIN               (1UL)
#define SESSION_DAYS_MAX               (365UL)

#define SESSION_SECONDS_PER_MINUTE     (60UL)
#define SESSION_SECONDS_PER_HOUR       (3600UL)
#define SESSION_SECONDS_PER_DAY        (86400UL)

#define SESSION_ON_SECONDS_MIN         (SESSION_ON_HOURS_MIN  * SESSION_SECONDS_PER_HOUR)
#define SESSION_ON_SECONDS_MAX         (SESSION_ON_HOURS_MAX  * SESSION_SECONDS_PER_HOUR)
#define SESSION_OFF_SECONDS_MAX        (SESSION_OFF_HOURS_MAX * SESSION_SECONDS_PER_HOUR)
#define SESSION_DURATION_SECONDS_MIN   (SESSION_DAYS_MIN      * SESSION_SECONDS_PER_DAY)
#define SESSION_DURATION_SECONDS_MAX   (SESSION_DAYS_MAX      * SESSION_SECONDS_PER_DAY)

typedef enum {
    SESSION_STATE_IDLE      = 0,
    SESSION_STATE_STIM_ON   = 1,
    SESSION_STATE_STIM_REST = 2
} SessionState_t;

/**
 * @brief  Fotografia del estado de la FSM para el comando STATUS.
 */
typedef struct {
    SessionState_t state;
    uint32_t       elapsed_in_state_s;   /* segundos en el estado actual      */
    uint32_t       elapsed_total_s;      /* segundos desde el arranque        */
    uint32_t       on_seconds;           /* parametros de la sesion armada    */
    uint32_t       off_seconds;
    uint32_t       duration_seconds;
    uint32_t       freq_hz[2];           /* [DAC_BANK_A], [DAC_BANK_B]        */
    uint16_t       amp_mv[SESSION_NUM_CHANNELS];
    uint8_t        well_mask;            /* bit i (0..7) = pocillo i+1 activo */
} SessionStatus_t;

/**
 * @brief  Vincula los perifericos que necesita la FSM y recupera la sesion
 *         guardada en backup si la hay. Deja la FSM en IDLE (0 V) si no hay
 *         sesion valida que reanudar.
 * @param  hrtc      Handle del RTC (calendario + registros de backup).
 * @param  ad_bank_a Handle AD5144 de los canales 1-4.
 * @param  ad_bank_b Handle AD5144 de los canales 5-8.
 * @note   AD5144, DacWaveform y CultureSelector deben estar ya inicializados.
 */
HAL_StatusTypeDef SessionFSM_Init(RTC_HandleTypeDef *hrtc,
                                  AD5144_Handle_t *ad_bank_a,
                                  AD5144_Handle_t *ad_bank_b);

/**
 * @brief  Comprueba los parametros de START sin tocar nada (para que el
 *         parser distinga un error de rango de un fallo de hardware).
 */
bool SessionFSM_StartParamsValid(uint32_t on_seconds,
                                 uint32_t off_seconds,
                                 uint32_t duration_seconds);

/**
 * @brief  Arma y arranca una sesion. Los tres tiempos van en SEGUNDOS: la
 *         conversion desde las horas/dias del comando START es tarea del
 *         parser. Parametros invalidos -> HAL_ERROR sin tocar el estado
 *         anterior (ver SessionFSM_StartParamsValid). Si el hardware falla
 *         al aplicar las salidas, la sesion se cancela con apagado seguro y
 *         se devuelve el estado de error del periferico.
 * @param  on_seconds       Duracion del tramo de estimulacion.
 * @param  off_seconds      Duracion del tramo de reposo (0 = sin reposo).
 * @param  duration_seconds Duracion total de la sesion.
 */
HAL_StatusTypeDef SessionFSM_Start(uint32_t on_seconds,
                                   uint32_t off_seconds,
                                   uint32_t duration_seconds);

/**
 * @brief  Para la sesion: 0 V en los 8 canales, DAC parado, pocillos
 *         desconectados, backup borrado y FSM en IDLE.
 */
HAL_StatusTypeDef SessionFSM_Stop(void);

/**
 * @brief  Motor de la FSM. Llamar periodicamente desde el bucle principal
 *         sano (junto al refresco del IWDG). Evalua el calendario del RTC y
 *         ejecuta las transiciones ON<->REST y el fin de sesion.
 */
void SessionFSM_Task(void);

/**
 * @brief  Fija la consigna de amplitud de un canal (1..8) en milivoltios.
 *         Se aplica al instante si la FSM esta en STIM_ON; en cualquier otro
 *         estado se guarda y se aplicara al entrar en el siguiente STIM_ON.
 *         target_mv > SESSION_AMP_MAX_MV -> HAL_ERROR. Si la escritura en el
 *         AD5144 falla, se conserva la consigna anterior y se devuelve el
 *         error. Con 0 mV el pocillo se lleva a GND aunque este en la
 *         mascara (el AD5144 no llega a 0 V por si solo).
 */
HAL_StatusTypeDef SessionFSM_SetChannelMillivolts(uint8_t channel, uint16_t target_mv);

/**
 * @brief  Fija la frecuencia de un banco. Mismo criterio de aplicacion
 *         diferida que SessionFSM_SetChannelMillivolts. Fuera de
 *         [DAC_FREQ_MIN_HZ, DAC_FREQ_MAX_HZ] -> HAL_ERROR.
 */
HAL_StatusTypeDef SessionFSM_SetBankFrequency(DacBank_t bank, uint32_t freq_hz);

/**
 * @brief  Conecta/desconecta un pocillo (1..8) de la senal. Mismo criterio
 *         de aplicacion diferida. En STIM_ON un pocillo de la mascara solo
 *         se conecta si su canal tiene amplitud > 0 aplicada en el AD5144.
 */
HAL_StatusTypeDef SessionFSM_SetWell(uint8_t channel, bool connected);

/**
 * @brief  Mantenimiento: graba en la EEPROM de los dos AD5144 el codigo de
 *         ganancia minima, para que arranquen en posicion segura al
 *         encenderse. Solo se permite en IDLE (HAL_BUSY en otro estado).
 *         Tarda unos 0,1-0,4 s.
 */
HAL_StatusTypeDef SessionFSM_StoreAmplifierSafeDefault(void);

/**
 * @brief  Estado actual de la FSM.
 */
SessionState_t SessionFSM_GetState(void);

/**
 * @brief  Rellena *out con la fotografia completa del estado para STATUS.
 *         No hace nada si out == NULL.
 */
void SessionFSM_GetStatus(SessionStatus_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_INC_SESSION_FSM_H_ */
