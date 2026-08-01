/**
  ******************************************************************************
  * @file    fall_detect.c
  * @brief   三段式跌倒检测状态机实现
  *
  * 状态转移：
  *   NORMAL ──(accel_sq < freefall_threshold)──▶ FREE_FALL
  *   FREE_FALL ──(accel_sq > impact_threshold)──▶ IMPACT
  *              ──(超时 impact_window_ms)──────▶ NORMAL
  *   IMPACT ──(持续静止 still_time_ms)─────────▶ MOTIONLESS
  *          ──(超时 impact_timeout_ms)──────────▶ NORMAL
  *   MOTIONLESS ──(自动复位)────────────────────▶ NORMAL
  *
  ******************************************************************************
  */
#include "fall_detect.h"

/* ---- 模块内部状态 ---- */
static FallState_t          state;
static FallDetect_Config_t  cfg;

static uint32_t             state_enter_tick;   /* 进入当前状态的时间      */
static uint32_t             impact_start_tick;  /* 进入 IMPACT 的时间（不重置） */

static uint32_t             last_accel_sq;      /* 最近一次计算的加速度平方和 */
static uint32_t             last_gyro_sq;       /* 最近一次计算的角速度平方和 */

/* ================================================================
 *  公开 API
 * ================================================================ */
/**
  * @brief  初始化状态机
  */
void FallDetect_Init(const FallDetect_Config_t *config)
{
    cfg   = *config;
    state = FALL_STATE_NORMAL;
    state_enter_tick  = 0;
    impact_start_tick = 0;
}

/**
  * @brief  运行时修改单个阈值参数
  * @param  id     参数 ID（见 FallDetect_ParamId_t）
  * @param  value  新值
  * @note   仅 RAM 生效，掉电恢复默认值
  */
void FallDetect_SetParam(FallDetect_ParamId_t id, uint32_t value)
{
    switch (id)
    {
        case FD_PARAM_FREEFALL_THRESHOLD:
            cfg.freefall_threshold = value;
            break;
        case FD_PARAM_IMPACT_THRESHOLD:
            cfg.impact_threshold = value;
            break;
        case FD_PARAM_STILL_LOW:
            cfg.still_low = value;
            break;
        case FD_PARAM_STILL_HIGH:
            cfg.still_high = value;
            break;
        default:
            break;
    }
}


/**
  * @brief  喂入一帧原始数据，驱动状态机
  *         内部计算 accel_sq / gyro_sq
  */
FallEvent_t FallDetect_Process(const MPU_Raw_t *raw, uint32_t now)
{
    uint32_t accel_sq = (uint32_t)raw->ax_raw * raw->ax_raw
                      + (uint32_t)raw->ay_raw * raw->ay_raw
                      + (uint32_t)raw->az_raw * raw->az_raw;
    uint32_t gyro_sq  = (uint32_t)raw->gx_raw * raw->gx_raw
                      + (uint32_t)raw->gy_raw * raw->gy_raw
                      + (uint32_t)raw->gz_raw * raw->gz_raw;
    FallEvent_t event = FALL_EVENT_NONE;

    last_accel_sq = accel_sq;
    last_gyro_sq  = gyro_sq;

    switch (state)
    {
            /* ======== NORMAL：等待失重 ======== */
            case FALL_STATE_NORMAL:
                if (accel_sq < cfg.freefall_threshold) 
                {
                    state = FALL_STATE_FREE_FALL;
                    state_enter_tick = now;
                    event = FALL_EVENT_FREEFALL;
                }
                break;
            /* ======== FREE_FALL：等待冲击（有时间窗） ======== */
            case FALL_STATE_FREE_FALL:
                if (accel_sq > cfg.impact_threshold && gyro_sq > cfg.gyro_threshold
                    && (now - state_enter_tick) <= cfg.impact_window_ms)
                {
                    /* 失重后窗口期内加速度+角速度都超阈值 → IMPACT */
                    state = FALL_STATE_IMPACT;
                    state_enter_tick  = now;
                    impact_start_tick = now;      /* ← 独立记录，永不重置 */
                    event = FALL_EVENT_IMPACT;
                } 
                else if ((now - state_enter_tick) > cfg.impact_window_ms) 
                {
                    state = FALL_STATE_NORMAL;
                    event = FALL_EVENT_TIMEOUT;
                }
                break;
            /* ======== IMPACT：等待持续静止（双重计时器） ======== */
            case FALL_STATE_IMPACT:
                /*
                * 计时器 A：静止持续计时
                */
                if (accel_sq > cfg.still_low && accel_sq < cfg.still_high) 
                {
                    if ((now - state_enter_tick) >= cfg.still_time_ms) 
                    {
                        state = FALL_STATE_MOTIONLESS;
                        state_enter_tick = now;
                        event = FALL_EVENT_FALL_CONFIRMED;
                    }
                }
                else
                {
                    state_enter_tick = now;
                }

                /*
                * 计时器 B：全局超时保护
                */
                if (event == FALL_EVENT_NONE && (now - impact_start_tick) > cfg.impact_timeout_ms)
                {
                    state = FALL_STATE_NORMAL;
                    event = FALL_EVENT_TIMEOUT;
                }
                break;
            /* ======== MOTIONLESS：跌倒已确认 ======== */
            case FALL_STATE_MOTIONLESS:
                /*
                 * 报警后保持一段时间不响应新跌倒
                 */
                if ((now - state_enter_tick) >= cfg.alarm_hold_ms)
                {
                    state = FALL_STATE_NORMAL;
                }
                
                break;

            default:
                state = FALL_STATE_NORMAL;
                break;
    }

    return event;
}
/**
  * @brief  查询当前状态（只读）
  */
FallState_t FallDetect_GetState(void)
{
    return state;
}
/**
  * @brief  强制复位到 NORMAL
  */
void FallDetect_Reset(void)
{
    state = FALL_STATE_NORMAL;
    state_enter_tick  = 0;
    impact_start_tick = 0;
}

uint32_t FallDetect_GetAccelSq(void)
{
    return last_accel_sq;
}

uint32_t FallDetect_GetGyroSq(void)
{
    return last_gyro_sq;
}
