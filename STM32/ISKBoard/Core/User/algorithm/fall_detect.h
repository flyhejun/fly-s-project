/**
  ******************************************************************************
  * @file    fall_detect.h
  * @brief   MPU6050 跌倒检测算法模块
  *          三段式跌倒模型：失重 → 冲击 → 静止
  *          使用方式：
  *            FallDetect_Init(&config);
  *            loop {
  *                input.accel_sq = ...;
  *                input.timestamp_ms = ...;
  *                event = FallDetect_Process(&input);
  *                // 根据 event 驱动 LED/蜂鸣器/BLE 等
  *            }
  ******************************************************************************
  */
#ifndef __FALL_DETECT_H
#define __FALL_DETECT_H

#include <stdint.h>

/* ================================================================
 *  状态机状态
 * ================================================================ */
typedef enum {
    FALL_STATE_NORMAL      = 0,   /* 正常状态，等待失重          */
    FALL_STATE_FREE_FALL,         /* 疑似失重（accel_sq 过低）   */
    FALL_STATE_IMPACT,            /* 检测到冲击（accel_sq 骤升）  */
    FALL_STATE_MOTIONLESS,        /* 冲击后静止 → 跌倒确认       */
} FallState_t;

/* ================================================================
 *  算法输出事件
 *  调用方根据事件决定做什么（printf / LED / BLE / ...）
 * ================================================================ */
typedef enum {
    FALL_EVENT_NONE           = 0, /* 无事件                      */
    FALL_EVENT_FREEFALL,           /* 检测到疑似失重              */
    FALL_EVENT_IMPACT,             /* 检测到冲击                  */
    FALL_EVENT_FALL_CONFIRMED,     /* 三段走完，跌倒确认          */
    FALL_EVENT_TIMEOUT,            /* 窗口超时，回退到 NORMAL     */
} FallEvent_t;

/* ================================================================
 *  算法输入
 * ================================================================ */
typedef struct {
    /* ---- 当前使用 ---- */
    uint32_t accel_sq;             /* 加速度平方和（ax²+ay²+az²） */
    uint32_t timestamp_ms;         /* 时间戳 (ms)                 */

    /* ---- gyro 角速度 (°/s) ---- */
    float    gyro_x_dps;           /* 绕 X 轴角速度               */
    float    gyro_y_dps;           /* 绕 Y 轴角速度               */
    float    gyro_z_dps;           /* 绕 Z 轴角速度               */

    /* ---- 预留：姿态角 (deg) ---- */
    float    pitch;                /* 俯仰角                      */
    float    roll;                 /* 横滚角                      */
} FallDetect_Input_t;

/* ================================================================
 *  可配置参数
 * ================================================================ */
typedef struct {
    uint32_t freefall_threshold;   /* 失重阈值（加速度平方和）    */
    uint32_t impact_threshold;     /* 冲击阈值（加速度平方和）    */
    uint32_t still_low;            /* 静止下限（加速度平方和）    */
    uint32_t still_high;           /* 静止上限（加速度平方和）    */
    uint32_t impact_window_ms;     /* 失重→冲击 最大时间窗       */
    uint32_t still_time_ms;        /* 冲击→静止 确认需持续多久   */
    uint32_t impact_timeout_ms;    /* IMPACT 全局超时保护         */
    uint32_t alarm_hold_ms;        /* 报警锁定期：跌倒确认后多长时间不响应新跌倒 */
} FallDetect_Config_t;

/* ================================================================
 *  API
 * ================================================================ */ 
void FallDetect_Init(const FallDetect_Config_t *config);

/**
  * @brief  喂入一帧 IMU 数据，驱动状态机
  * @param  input  预处理后的数据（加速度平方和 + 时间戳）
  * @retval 本次调用产生的事件
  */
FallEvent_t FallDetect_Process(const FallDetect_Input_t *input);

/**
  * @brief  查询当前状态（只读）
  */
FallState_t FallDetect_GetState(void);

/**
  * @brief  强制复位到 NORMAL（例如外部按键取消报警）
  */
void FallDetect_Reset(void);

#endif /* __FALL_DETECT_H */
