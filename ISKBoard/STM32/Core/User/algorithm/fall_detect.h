/**
  ******************************************************************************
  * @file    fall_detect.h
  * @brief   MPU6050 跌倒检测算法模块
  *          三段式跌倒模型：失重 → 冲击 → 静止
  *          使用方式：
  *            FallDetect_Init(&(FallDetect_Config_t)FALL_DETECT_DEFAULT_CONFIG);
  *            loop {
  *                event = FallDetect_Process(&mpu_raw, timestamp_ms);
  *                // 根据 event 驱动 LED/蜂鸣器/BLE 等
  *            }
  ******************************************************************************
  */
#ifndef __FALL_DETECT_H
#define __FALL_DETECT_H

#include <stdint.h>
#include "mpu6050.h"

/* ================================================================
 *  默认阈值
 * ================================================================ */
#define FALL_DETECT_DEFAULT_CONFIG {                                   \
    .freefall_threshold = 70000000U,   /* 约 0.51g                 */ \
    .impact_threshold   = 1000000000U,  /* 约 1.93g                 */ \
    .still_low          = 130000000U,  /* 约 0.7g                  */ \
    .still_high         = 450000000U,  /* 约 1.3g                  */ \
    .impact_window_ms   = 800,          /* 失重→冲击窗口           */ \
    .still_time_ms      = 2000,         /* 静止确认时长             */ \
    .impact_timeout_ms  = 5000,         /* IMPACT 全局超时          */ \
    .alarm_hold_ms      = 15000,        /* 报警锁定期               */ \
    .gyro_threshold     = 200000000U,   /* 约 108°/s 等效角速度    */ \
}

/* ================================================================
 *  状态机状态
 * ================================================================ */
typedef enum {
    FALL_STATE_NORMAL      = 0,
    FALL_STATE_FREE_FALL,
    FALL_STATE_IMPACT,
    FALL_STATE_MOTIONLESS,
} FallState_t;

/* ================================================================
 *  算法输出事件
 * ================================================================ */
typedef enum {
    FALL_EVENT_NONE           = 0,
    FALL_EVENT_FREEFALL,
    FALL_EVENT_IMPACT,
    FALL_EVENT_FALL_CONFIRMED,
    FALL_EVENT_TIMEOUT,
} FallEvent_t;

/* ================================================================
 *  可配置参数
 * ================================================================ */
typedef struct {
    uint32_t freefall_threshold;
    uint32_t impact_threshold;
    uint32_t still_low;
    uint32_t still_high;
    uint32_t impact_window_ms;
    uint32_t still_time_ms;
    uint32_t impact_timeout_ms;
    uint32_t alarm_hold_ms;
    uint32_t gyro_threshold;
} FallDetect_Config_t;

/* ================================================================
 *  可运行修改的阈值参数 ID（仅阈值，时间/gyro 只读）
 * ================================================================ */
typedef enum {
    FD_PARAM_FREEFALL_THRESHOLD = 0x01,
    FD_PARAM_IMPACT_THRESHOLD   = 0x02,
    FD_PARAM_STILL_LOW          = 0x03,
    FD_PARAM_STILL_HIGH         = 0x04,
} FallDetect_ParamId_t;

/* ================================================================
 *  API
 * ================================================================ */
void FallDetect_Init(const FallDetect_Config_t *config);

/**
  * @brief  运行时修改单个阈值参数
  * @param  id     参数 ID（见 FallDetect_ParamId_t）
  * @param  value  新值
  * @note   仅 RAM 生效，掉电恢复默认值
  */
void FallDetect_SetParam(FallDetect_ParamId_t id, uint32_t value);


FallEvent_t FallDetect_Process(const MPU_Raw_t *raw, uint32_t timestamp_ms);

/* 取最近一次计算的 accel_sq / gyro_sq */
uint32_t FallDetect_GetAccelSq(void);
uint32_t FallDetect_GetGyroSq(void);

/**
  * @brief  查询当前状态（只读）
  */
FallState_t FallDetect_GetState(void);

/**
  * @brief  强制复位到 NORMAL（例如外部按键取消报警）
  */
void FallDetect_Reset(void);

#endif /* __FALL_DETECT_H */
