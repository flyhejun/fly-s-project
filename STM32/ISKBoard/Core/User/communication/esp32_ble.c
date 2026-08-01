/**
  ******************************************************************************
  * @file    esp32_ble.c
  * @brief   ESP32 BLE 透传驱动实现
  *
  * ESP32_Init 留给你来实现，思路是：
  *   1. 发 "AT\r\n"
  *   2. 收 ESP32 回应的字符串
  *   3. 检查是否包含 "OK"
  *   4. 返回 0=成功 / 1=失败
  *
  * ESP32_Send 已经实现，直接调用 HAL_UART_Transmit。
  ******************************************************************************
  */
#include "esp32_ble.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

/* ================================================================
 *  ESP32_Init — 检测 ESP32 是否在线
 *
 *  发 "AT\r\n" → ESP32 回应 "AT\r\n\r\nOK\r\n" → 检查 "OK"
 *  重试 3 次，任意一次成功就返回 0。
 * ================================================================ */
uint8_t ESP32_Init(void)
{
    uint8_t buf[64];
    uint8_t retry = 3;

    while (retry--) {
        HAL_UART_Transmit(&huart2, (uint8_t *)"AT\r\n", 4, 100);
        HAL_Delay(200);
        memset(buf, 0, sizeof(buf));
        if (HAL_UART_Receive(&huart2, buf, sizeof(buf)-1, 500) == HAL_OK
            && strstr((char *)buf, "OK") != NULL) 
        {
            return 0;
        }
    }
    return 1;
}

/* ================================================================
 *  ESP32_Send — 通过 USART2 发送数据给 ESP32
 *
 *  block 模式发送，发完返回。
 *  后续如果需要大帧（EVENT_FULL = 6150B），可改为 DMA。
 * ================================================================ */
void ESP32_Send(const uint8_t *buf, uint16_t len)
{
   if(HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, 1000) == HAL_OK)
   {
        printf("data from stm to esp OK \n");
   }
    
}
