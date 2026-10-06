/**
 ******************************************************************************
 * @file    uart_protocol.h
 * @brief   Parser de comandos de texto del host sobre USB CDC (puerto serie
 *          virtual).
 *
 * Protocolo: una linea por comando, terminada en '\n' (se tolera '\r'). El
 * firmware responde SIEMPRE con una linea:
 *
 *   OK\n                      -> comando aceptado y aplicado.
 *   ERR <motivo>\n            -> comando rechazado; el estado anterior no se
 *                               ha tocado.
 *
 * Motivos de error:
 *   ERR SYNTAX   comando desconocido, numero de argumentos incorrecto o
 *                token mal formado (p.ej. texto donde se espera un numero).
 *   ERR RANGE    argumento numerico fuera del rango permitido. Requisito de
 *                seguridad del proyecto: no se recorta ni se aplica a medias.
 *   ERR HW       el valor era valido pero el periferico fallo al aplicarlo
 *                (fallo de bus I2C con el AD5144, escritura de flash, etc.).
 *   ERR STATE    la orden no se permite en el estado actual (p. ej. una
 *                operacion de mantenimiento con una sesion en marcha).
 *
 * Comandos (ver capitulo de Requisitos de diseno de la memoria):
 *
 *   SET AMP  <canal 1-8> <mV 0-2000>        amplitud de un canal
 *   SET FREQ <banco A|B>  <Hz 1-300000>     frecuencia de un banco
 *   SET WELL <canal 1-8>  <ON|OFF>          conecta/desconecta un pocillo
 *   START <horas_on> <horas_off> <dias>     arma y arranca la sesion
 *   STOP                                    para todo, 0 V, FSM a IDLE
 *   STATUS                                  vuelca estado + parametros
 *
 * Calibracion de la medida de corriente (ver current_calc.h). Los decimales
 * usan punto; hasta 6 cifras decimales:
 *
 *   SET CAL <canal 1-8> <ganancia> <offset_uA>   ganancia en (0, 10],
 *                                                offset en +/-2000 uA
 *   SET VREF <mV 1-3600>                         VREF real de la placa
 *   GET CAL <canal 1-8>                          linea de datos + OK:
 *       CAL CH=<n> GAIN=<g> OFFSET_UA=<o> VREF_MV=<v>
 *   SAVE CAL                                     guarda VREF y los 8 canales
 *                                                en flash (persisten)
 *
 * Medida de corriente (ver sd_logger.h):
 *
 *   GET CUR          linea de datos + OK:
 *       CUR LOG=<OFF|ON|NOSD> FILE=<BNNNN.BIN|-> REC=<n> UA=<v1>,...,<v8>
 *     con el valor de cada canal del ultimo segundo en uA (3 decimales, NAN
 *     si no hay medida). LOG=NOSD: se mide pero no hay tarjeta o fallo la
 *     escritura.
 *
 * Mantenimiento (solo en IDLE):
 *
 *   EEPROM SAFE      graba el codigo de ganancia minima en la EEPROM de los
 *                    dos AD5144 (arranque seguro al encender). Gasta ciclos
 *                    de EEPROM: usar una vez por placa, no de forma rutinaria.
 *
 * Respuesta de STATUS (linea de datos antes del OK\n):
 *
 *   STATE=<IDLE|STIM_ON|STIM_REST> T_STATE=<s> T_TOTAL=<s> ON=<s> OFF=<s>
 *   DUR=<s> FREQ_A=<Hz> FREQ_B=<Hz> AMP=<mV x8 separados por coma>
 *   WELL=0x<mascara hex, bit i = pocillo i+1>
 *
 * Las palabras clave se reconocen sin distinguir mayusculas/minusculas.
 *
 * Esta capa NO reescribe la pila USB: usa CDC_Transmit_FS() para enviar y se
 * alimenta desde el callback CDC_Receive_FS() (ver UartProtocol_RxData).
 ******************************************************************************
 */
#ifndef APP_INC_UART_PROTOCOL_H_
#define APP_INC_UART_PROTOCOL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Reinicia el estado del parser (buffers de recepcion y de linea).
 *         Llamar una vez tras SessionFSM_Init y antes del bucle principal.
 */
void UartProtocol_Init(void);

/**
 * @brief  Entrega al parser los bytes recibidos por USB CDC. Pensada para
 *         llamarse desde CDC_Receive_FS(), es decir, en contexto de
 *         interrupcion: solo copia a un buffer circular, no parsea. Los
 *         bytes que no quepan se descartan (el host reintenta la linea).
 * @param  data  Puntero a los bytes recibidos.
 * @param  len   Numero de bytes.
 */
void UartProtocol_RxData(const uint8_t *data, uint32_t len);

/**
 * @brief  Procesa los bytes pendientes: reconstruye lineas completas,
 *         interpreta el comando y envia la respuesta. Llamar de forma
 *         periodica desde el bucle principal sano (junto al refresco del
 *         IWDG y a SessionFSM_Task).
 */
void UartProtocol_Task(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_INC_UART_PROTOCOL_H_ */
