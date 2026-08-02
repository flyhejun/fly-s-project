#ifndef __BSP_SOFT_I2C_H
#define __BSP_SOFT_I2C_H

#include "main.h"

#define SOFT_I2C_DELAY_US  5   /* 默认 5μs ≈ 100kHz SCL */
/* 软件I2C引脚 */
#define SOFT_I2C_SCL_PORT     GPIOB
#define SOFT_I2C_SCL_PIN      GPIO_PIN_10

#define SOFT_I2C_SDA_PORT     GPIOB
#define SOFT_I2C_SDA_PIN      GPIO_PIN_11



void SOFT_I2C_Init(void);

void SOFT_I2C_Start(void);

void SOFT_I2C_Stop(void);

uint8_t SOFT_I2C_WaitAck(void);

void SOFT_I2C_SendAck(void);

void SOFT_I2C_SendNack(void);

void SOFT_I2C_SendByte(uint8_t data);

uint8_t SOFT_I2C_ReadByte(uint8_t ack);

/* I2C 通用操作（封装了设备地址+寄存器地址的完整时序） */
void SOFT_I2C_WriteByteTo(uint8_t dev_addr, uint8_t reg_addr, uint8_t data);

uint8_t SOFT_I2C_ReadBytesFrom(uint8_t dev_addr, uint8_t reg_addr, uint8_t *buf, uint8_t len);

#endif