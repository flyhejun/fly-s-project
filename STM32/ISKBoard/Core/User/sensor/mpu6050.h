#ifndef __MPU6050_H
#define __MPU6050_H

#include "main.h"


/* ==================== MPU6050 硬件参数 ==================== */

#define MPU6050_ADDR         0x68    // I2C 设备地址（AD0 引脚接地）


/* ==================== MPU6050 寄存器地址 ==================== */

/*
 * MPU6050 内部有 100+ 个寄存器，每个寄存器控制不同功能。
 * 我们用到的关键寄存器如下：
 */

#define MPU6050_REG_WHO_AM_I         0x75  // 芯片 ID 寄存器（读出来应该是 0x68）
#define MPU6050_REG_PWR_MGMT_1       0x6B  // 电源管理寄存器 1
#define MPU6050_REG_SAMPLE_RATE_DIV  0x19  // 采样率分频器
#define MPU6050_REG_DLPF_CONFIG      0x1A  // 数字低通滤波器配置
#define MPU6050_REG_GYRO_CONFIG      0x1B  // 陀螺仪量程配置
#define MPU6050_REG_ACCEL_CONFIG     0x1C  // 加速度计量程配置

/*
 * 加速度数据从 0x3B 开始，连续 6 个字节：
 *   AX_H, AX_L, AY_H, AY_L, AZ_H, AZ_L
 * 陀螺仪数据从 0x43 开始，连续 6 个字节：
 *   GX_H, GX_L, GY_H, GY_L, GZ_H, GZ_L
 * 一次性连续读 14 字节可以获取全部 6 轴数据，I2C 只需要一次传输
 */
#define MPU6050_REG_ACCEL_DATA_START  0x3B  // 加速度数据起始地址
#define MPU6050_REG_GYRO_DATA_START   0x43  // 陀螺仪数据起始地址

/* ==================== 量程与灵敏度 ==================== */
/* 当前配置：加速度计 ±2g（16384 LSB/g），陀螺仪 ±250°/s（131 LSB/(°/s)） */
#define MPU6050_ACCEL_RANGE_2G        0x00
#define MPU6050_GYRO_RANGE_250DPS     0x00
#define MPU6050_ACCEL_SCALE           16384
#define MPU6050_GYRO_SCALE            131

/* ==================== 原始数据结构 ==================== */

/*
 * Raw 表示 ADC 原始值（-32768 ~ +32767）
 * 需要除以灵敏度才能得到物理单位 (g 或 °/s)
 */
typedef struct
{
    int16_t ax_raw;
    int16_t ay_raw;
    int16_t az_raw;
    int16_t gx_raw;
    int16_t gy_raw;
    int16_t gz_raw;
} MPU_Raw_t;

/* ==================== 函数声明 ==================== */

/* 读取 WHO_AM_I 寄存器，确认芯片通信正常。应返回 0x68 */
uint8_t MPU6050_ReadID(void);

/*
 * MPU6050 初始化：
 *   1. 唤醒芯片（退出睡眠模式）
 *   2. 配置加速度计量程（默认 ±2g）
 *   3. 配置陀螺仪量程（默认 ±250°/s）
 *   4. 配置采样率和低通滤波
 */
void MPU6050_Init(void);

/* 从 0x3B 和 0x43 一次性读取全部 6 轴原始数据（14 字节） */
uint8_t MPU6050_ReadAll(MPU_Raw_t *data);


#endif