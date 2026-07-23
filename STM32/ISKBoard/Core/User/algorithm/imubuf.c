/**
  ******************************************************************************
  * @file    imubuf.c
  * @brief   IMU 数据环形缓存实现
  *
  * 环形缓冲区容量 256 帧（2^8，索引用掩码）。
  *
  *   触发前：缓冲区始终保留最新的 256 帧，循环覆盖。
  *   触发后：再写入 150 帧，然后冻结。
  *   冻结后：缓冲区中保留 106 帧触发前数据 + 150 帧触发后数据 = 256 帧。
  *
  * 帧布局（冻结状态下）：
  *                       触发帧
  *         ┌───────────────┼────────────────────────────┐
  *         106 帧触发前     │     150 帧触发后             │
  *        (offset -105..0) │     (offset +1..+150)       │
  *                        ──
  *                        采样时间轴 →
  *
  * 存储开销：256 × 24 = 6,144 字节（≈ 6KB）
  ******************************************************************************
  */
#include "imubuf.h"
#include <stddef.h>
#include <stdio.h>
/* ================================================================
 *  常量
 * ================================================================ */
#define IMUBUF_SIZE         256     /* 总帧数（2^N，用 & 掩码实现循环） */
#define IMUBUF_MASK         (IMUBUF_SIZE - 1)
#define IMUBUF_POST_CNT     150     /* 触发后继续写入帧数                       */
#define IMUBUF_PRE_CNT      (IMUBUF_SIZE - IMUBUF_POST_CNT)  /* 触发前可回溯帧数 = 106 */

/* ================================================================
 *  内部状态
 * ================================================================ */
static IMU_Sample_t s_buffer[IMUBUF_SIZE];  /* 环形缓冲区，24 字节/帧 */
static uint32_t     s_wp;                   /* 写入位置（循环）        */
static uint32_t     s_mark;                 /* 触发时的 s_wp          */
static uint8_t      s_triggered;            /* 0=正常, 1=触发中, 2=已冻结 */
static uint8_t      s_event_id;             /* 触发时的事件 ID（预留）  */
static uint32_t     s_post_cnt;             /* 触发后已写入帧数        */

/* ================================================================
 *  API 实现
 * ================================================================ */
/**
  * @brief  初始化环形缓存（清零所有状态）
  */
void IMUBuf_Init(void)
{
    s_wp         = 0;
    s_mark       = 0;
    s_triggered  = 0;
    s_event_id   = 0;
    s_post_cnt   = 0;
}

/**
  * @brief  压入一帧 IMU 数据
  *
  * 正常态：写入当前位置，wp 前移，循环覆盖旧帧。
  * 触发态：继续写入，计数递增，够 IMUBUF_POST_CNT 后冻结。
  * 冻结态：静默丢弃。
  */
void IMUBuf_PushData(uint32_t timestamp_ms, const MPU_Raw_t *raw, uint32_t accel_sq, uint32_t gyro_sq)
{
    IMU_Sample_t *s;

    /* 已冻结，不再写入 */
    if (s_triggered == 2) {
        return;
    }

    /* 组装并写入当前槽 */
    s = &s_buffer[s_wp];
    s->accel_sq     = accel_sq;
    s->gyro_sq      = gyro_sq;
    s->timestamp_ms = timestamp_ms;
    s->ax_raw       = raw->ax_raw;
    s->ay_raw       = raw->ay_raw;
    s->az_raw       = raw->az_raw;
    s->gx_raw       = raw->gx_raw;
    s->gy_raw       = raw->gy_raw;
    s->gz_raw       = raw->gz_raw;

    s_wp = (s_wp + 1) & IMUBUF_MASK;

    /* 触发态下计数 */
    if (s_triggered == 1) {
        s_post_cnt++;
        if (s_post_cnt >= IMUBUF_POST_CNT) {
            s_triggered = 2;    /* 冻结 */
        }
    }
}

/**
  * @brief  标记事件点
  *
  * 记下当前 wp 作为事件后的第一帧写入位置。
  * 上一帧（s_wp - 1）就是触发帧（offset = 0）。
  */
void IMUBuf_Trigger(uint8_t event_id)
{
    if (s_triggered != 0) {
        return;
    }

    s_mark       = s_wp;
    s_event_id   = event_id;
    s_triggered  = 1;
    s_post_cnt   = 0;
}

/**
  * @brief  通过串口导出全部事件数据
  *
  * 输出 256 行 CSV（ts, accel_sq, gyro_sq）
  * 在报警期间或结束后调用，数据就绪时一次打完。
  */
void IMUBuf_Dump(void)
{
    int32_t             min_off;
    int32_t             max_off;
    int32_t             i;
    uint32_t            pos;
    const IMU_Sample_t *s;

    if (s_triggered != 2) {
        printf("[IMUBuf] no event data\n");
        return;
    }

    min_off = -(int32_t)(IMUBUF_PRE_CNT - 1);   /* -105 */
    max_off = (int32_t)IMUBUF_POST_CNT;          /* +150 */

    printf("[IMUBuf] event dump (%ld frames):\n"
           "ts,accel_sq,gyro_sq\n",
           (long)(max_off - min_off + 1));

    for (i = min_off; i <= max_off; i++) {
        pos = (s_mark - 1 + i + IMUBUF_SIZE) & IMUBUF_MASK;
        s = &s_buffer[pos];
        printf("%lu,%lu,%lu\n",
               (unsigned long)s->timestamp_ms,
               (unsigned long)s->accel_sq,
               (unsigned long)s->gyro_sq);
    }

    printf("[IMUBuf] end\n");
}

/**
  * @brief  填充统一跌倒事件（需 buffer 已冻结，否则字段为 0）
  */
void IMUBuf_GetPeak(FallEvent_Data_t *event)
{
    int32_t  i;
    uint32_t pos;
    uint32_t val;
    uint32_t trigger_pos;

    if (event == NULL) return;

    /* 未冻结则清零返回 */
    if (s_triggered != 2) {
        event->timestamp_ms     = 0;
        event->event_type       = 0;
        event->max_accel_sq     = 0;
        event->freefall_min_sq  = 0;
        event->samples          = NULL;
        return;
    }

    /* 触发帧的时间戳和事件类型 */
    trigger_pos = (s_mark - 1 + IMUBUF_SIZE) & IMUBUF_MASK;
    event->timestamp_ms = s_buffer[trigger_pos].timestamp_ms;
    event->event_type   = s_event_id;

    /* 扫描 256 帧，找峰值和谷值 */
    event->freefall_min_sq = 0xFFFFFFFF;
    event->max_accel_sq    = 0;

    for (i = -(int32_t)(IMUBUF_PRE_CNT - 1); i <= (int32_t)IMUBUF_POST_CNT; i++) {
        pos = (s_mark - 1 + i + IMUBUF_SIZE) & IMUBUF_MASK;
        val = s_buffer[pos].accel_sq;

        if (val < event->freefall_min_sq)
            event->freefall_min_sq = val;
        if (val > event->max_accel_sq)
            event->max_accel_sq = val;
    }

    /* 指向环形缓冲区，零拷贝 */
    event->samples = s_buffer;
}

/**
  * @brief  复位到写入模式
  *
  * 将 wp 置为 s_mark（冻结时的下一写入位置），
  * 这样旧事件数据会随着新写入逐渐被覆盖，不需要显式清零。
  */
void IMUBuf_Reset(void)
{
    s_triggered  = 0;
    s_wp         = s_mark;   /* 从冻结位置继续循环 */
    s_post_cnt   = 0;
}
