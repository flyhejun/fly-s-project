#ifndef __BSP_SOFT_I2C_H
#define __BSP_SOFT_I2C_H

#include "main.h"

#define I2C_DELAY_US  5   /* 默认 5μs ≈ 100kHz SCL */
/* 软件I2C引脚 */
#define SOFT_I2C_SCL_PORT     GPIOB
#define SOFT_I2C_SCL_PIN      GPIO_PIN_10

#define SOFT_I2C_SDA_PORT     GPIOB
#define SOFT_I2C_SDA_PIN      GPIO_PIN_11



void I2C_Init(void);

void I2C_Start(void);

void I2C_Stop(void);

uint8_t I2C_WaitAck(void);

void I2C_SendAck(void);

void I2C_SendNack(void);

void I2C_SendByte(uint8_t data);

uint8_t I2C_ReadByte(uint8_t ack);

/* I2C 通用操作（封装了设备地址+寄存器地址的完整时序） */
void I2C_WriteOneByte(uint8_t dev_addr, uint8_t reg_addr, uint8_t data);

void I2C_ReadBytes(uint8_t dev_addr, uint8_t reg_addr, uint8_t *buf, uint8_t len);

#endif