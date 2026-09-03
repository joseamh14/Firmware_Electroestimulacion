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
 *                (fallo de bus I2C con el AD5144, etc.).
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
