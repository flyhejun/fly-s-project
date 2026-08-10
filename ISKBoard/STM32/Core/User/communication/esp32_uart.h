/**
  ******************************************************************************
  * @file    esp32_uart.h
  * @brief   ESP32 UART 驱动 — 通过 USART2 向 ESP32 透传数据
  ******************************************************************************
  */
#ifndef __ESP32_UART_H
#define __ESP32_UART_H

#include <stdint.h>

/* BLE 初始化是否完成（commTask 据此决定是否操作 ESP32） */
extern volatile uint8_t g_ble_ready;

/* AT+BLEADVDATA 运行时错误标志（ESP32 返回 ERROR 时由 RX 解析置位） */
extern volatile uint8_t g_ble_advdata_err;

/* BLE 初始化状态机：commTask 每轮调一步，分步推进 */
void ESP32_Init_BLE_Step(void);

/* 重置 BLE 状态机（运行时报错触发重初始化时调用） */
void ESP32_Reset_BLE(void);

void ESP32_Send(const uint8_t *buf, uint16_t len);
void ESP32_CheckAdvStatus(void);
void ESP32_RX_Char(uint8_t ch);
void ESP32_RX_Poll(void);

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
