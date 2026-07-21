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
  * @brief  喂入一帧数据，驱动状态机
  */
FallEvent_t FallDetect_Process(const FallDetect_Input_t *input)
{
    uint32_t   accel_sq = input->accel_sq;
    uint32_t   now      = input->timestamp_ms;
    FallEvent_t event   = FALL_EVENT_NONE;

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
                if (accel_sq > cfg.impact_threshold &&(now - state_enter_tick) <= cfg.impact_window_ms) 
                {
                    /* 失重后窗口期内检测到冲击 → 进入 IMPACT */
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
                if (event == FALL_EVENT_NONE &&(now - impact_start_tick) > cfg.impact_timeout_ms) 
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
                if((now - state_enter_tick) >= cfg.alarm_hold_ms)
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
