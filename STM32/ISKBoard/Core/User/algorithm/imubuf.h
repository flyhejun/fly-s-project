/**
  ******************************************************************************
  * @file    imubuf.h
  * @brief   IMU 数据环形缓存 —— 跌倒事件触发前后数据保存
  *
  * 设计思路：
  *   1. 256 帧环形缓冲区，始终写入，循环覆盖最旧帧
  *   2. IMUBuf_Trigger() 标记事件位置，再写入 150 帧后冻结
  *   3. IMUBuf_GetPeak() 提取事件峰值+零拷贝样本指针
  *   4. IMUBuf_Reset() 解锁，回到写入模式
  *
  * 使用方式：
  *   for (每帧) {
  *       IMUBuf_PushData(ts, &raw, accel_sq);  // 始终写入
  *       if (跌倒确认) IMUBuf_Trigger(0);        // 标记事件
  *   }
  *   IMUBuf_GetPeak(&event);  // 报警期间提取事件数据
  *   IMUBuf_Reset();          // 回到写入模式
  *
  * 零 RTOS 依赖，纯数据入/数据出。
  ******************************************************************************
  */
#ifndef __IMUBUF_H
#define __IMUBUF_H

#include <stdint.h>
#include "mpu6050.h"

/* ================================================================
 *  IMU 样本结构体
 * ================================================================ */
typedef struct {
    uint32_t timestamp_ms;              /* 采样时间戳 (ms)       */

    /* 原始 ADC 值 */
    int16_t  ax_raw;
    int16_t  ay_raw;
    int16_t  az_raw;
    int16_t  gx_raw;
    int16_t  gy_raw;
    int16_t  gz_raw;

    /* 预处理结果 */
    uint32_t accel_sq; 
    uint32_t gyro_sq;                 
} IMU_Sample_t;

/* ================================================================
 *  统一跌倒事件
 * ================================================================ */
typedef struct {
    uint32_t timestamp_ms;       /* 事件发生时间戳 (ms)                     */
    uint8_t  event_type;         /* 事件类型：0=跌倒确认（预留扩展）         */

    uint32_t max_accel_sq;       /* 冲击阶段加速度峰值                      */
    uint32_t freefall_min_sq;    /* 失重阶段加速度谷值                      */

    const IMU_Sample_t *samples; /* 指向环形缓冲区，零拷贝                   */
} FallEvent_Data_t;
/* ================================================================
 *  API
 * ================================================================ */

void IMUBuf_Init(void);

void IMUBuf_PushData(uint32_t timestamp_ms, const MPU_Raw_t *raw, uint32_t accel_sq, uint32_t gyro_sq);

void IMUBuf_Trigger(uint8_t event_id);

void IMUBuf_GetPeak(FallEvent_Data_t *event);

void IMUBuf_Reset(void);

#endif /* __IMUBUF_H */
