/**
  ******************************************************************************
  * @file    esp32_uart.h
  * @brief   ESP32 UART 驱动 — 通过 USART2 向 ESP32 透传数据
  ******************************************************************************
  */
#ifndef __ESP32_UART_H
#define __ESP32_UART_H

#include <stdint.h>

/* BLE 初始化状态 */
typedef enum {
    BLE_INIT_IDLE = 0,
    BLE_INIT_OK,        /* 全部 AT 命令成功 */
    BLE_INIT_FAIL,      /* 某条命令超时或返回 ERROR */
} BLE_Init_Status_t;

extern volatile uint8_t g_ble_connected;

void ESP32_Send(const uint8_t *buf, uint16_t len);
void ESP32_CheckPending(void);
void ESP32_RX_Char(uint8_t ch);
BLE_Init_Status_t ESP32_Init_BLE(void);

/**
  * @brief  取回一帧完整的下行指令（若有）
  * @param  buf  输出：帧数据（含 SOF/EOF）
  * @param  len  输出：帧长
  * @retval 1  取到一帧（buf 已填充）
  *         0  无完整帧
  * @note   应在任务上下文中轮询调用（非中断）
  */
uint8_t ESP32_RX_GetFrame(uint8_t *buf, uint16_t *len);

#endif /* __ESP32_UART_H */
