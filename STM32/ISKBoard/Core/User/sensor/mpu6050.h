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

/* ==================== 量程与灵敏度对照 ==================== */

/*
 * 加速度计量程选择（写入 ACCEL_CONFIG 寄存器的值）：
 *   0x00 → ±2g   → 灵敏度 16384 LSB/g  ← 跌倒检测推荐用这个
 *   0x08 → ±4g   → 灵敏度  8192 LSB/g
 *   0x10 → ±8g   → 灵敏度  4096 LSB/g
 *   0x18 → ±16g  → 灵敏度  2048 LSB/g
 */
#define MPU6050_ACCEL_RANGE_2G        0x00
#define MPU6050_ACCEL_RANGE_4G        0x08
#define MPU6050_ACCEL_RANGE_8G        0x10
#define MPU6050_ACCEL_RANGE_16G       0x18

/* 加速度灵敏度：原始值 ÷ 灵敏度 = g 值 */
typedef enum {
    MPU6050_ACCEL_SCALE_2G  = 16384,
    MPU6050_ACCEL_SCALE_4G  = 8192,
    MPU6050_ACCEL_SCALE_8G  = 4096,
    MPU6050_ACCEL_SCALE_16G = 2048,
} MPU6050_AccelScale_t;

/*
 * 陀螺仪量程选择（写入 GYRO_CONFIG 寄存器的值）：
 *   0x00 →  ±250°/s  → 灵敏度 131   LSB/(°/s)
 *   0x08 →  ±500°/s  → 灵敏度 65.5  LSB/(°/s)
 *   0x10 → ±1000°/s  → 灵敏度 32.8  LSB/(°/s)
 *   0x18 → ±2000°/s  → 灵敏度 16.4  LSB/(°/s)
 */
#define MPU6050_GYRO_RANGE_250DPS      0x00
#define MPU6050_GYRO_RANGE_500DPS      0x08
#define MPU6050_GYRO_RANGE_1000DPS     0x10
#define MPU6050_GYRO_RANGE_2000DPS     0x18

/* 陀螺仪灵敏度：原始值 ÷ 灵敏度 = °/s */
typedef enum {
    MPU6050_GYRO_SCALE_250DPS  = 131,
    MPU6050_GYRO_SCALE_500DPS  = 66,   // 实际是 65.5，取整
    MPU6050_GYRO_SCALE_1000DPS = 33,   // 实际是 32.8，取整
    MPU6050_GYRO_SCALE_2000DPS = 16,   // 实际是 16.4，取整
} MPU6050_GyroScale_t;

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
} MPU6050_RawData_t;

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
void MPU6050_ReadAll(MPU6050_RawData_t *data);


#endif