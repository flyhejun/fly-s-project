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
static uint32_t     s_seq;                  /* 全局递增序列号          */
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
    s_seq        = 0;
    s_triggered  = 0;
    s_event_id   = 0;
    s_post_cnt   = 0;
}

void IMUBuf_PushData(uint32_t timestamp_ms, const MPU6050_RawData_t *raw, uint32_t accel_sq)
{
    IMU_Sample_t    sample;

    sample.accel_sq = accel_sq;
    sample.timestamp_ms =timestamp_ms;
    sample.ax_raw = raw->ax_raw;
    sample.ay_raw = raw->ay_raw;
    sample.az_raw = raw->az_raw;
    sample.gx_raw = raw->gy_raw;
    sample.gz_raw = raw->gz_raw;
    sample.seq = 0;
    
    IMUBuf_Push(&sample);
}
/**
  * @brief  压入一帧
  *
  * 正常态：写入当前位置，wp 前移，循环覆盖旧帧。
  * 触发态：继续写入，计数递增，够 IMUBUF_POST_CNT 后冻结。
  * 冻结态：静默丢弃（Push 不写任何东西）。
  */
void IMUBuf_Push(const IMU_Sample_t *sample)
{
    /* 已冻结，不再写入 */
    if (s_triggered == 2) {
        return;
    }

    /* 写入当前槽，内部拷贝 */
    s_buffer[s_wp] = *sample;
    s_buffer[s_wp].seq = s_seq;
    s_seq++;

    /* wp 前移（循环） */
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
    /* 已触发过，忽略重复 */
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
  * 输出 256 行 CSV（offset, seq, ts, ax, ay, az, gx, gy, gz, accel_sq）
  * 在报警期间或结束后调用，数据就绪时一次打完。
  */
void IMUBuf_DumpAll(void)
{
    int32_t min_off;
    int32_t max_off;
    int32_t i;

    if (s_triggered != 2) {
        printf("[IMUBuf] no event data\n");
        return;
    }

    min_off = -(int32_t)(IMUBUF_PRE_CNT - 1);   /* -105 */
    max_off = (int32_t)IMUBUF_POST_CNT;          /* +150 */

    printf("[IMUBuf] event dump (%ld frames):\n"
           "offset,seq,ts,ax,ay,az,gx,gy,gz,accel_sq\n",
           (long)(max_off - min_off + 1));

    for (i = min_off; i <= max_off; i++) {
        uint32_t pos = (s_mark - 1 + i + IMUBUF_SIZE) & IMUBUF_MASK;
        const IMU_Sample_t *s = &s_buffer[pos];
        printf("%+4ld,%lu,%lu,%d,%d,%d,%d,%d,%d,%lu\n",
               (long)i,
               (unsigned long)s->seq, (unsigned long)s->timestamp_ms,
               s->ax_raw, s->ay_raw, s->az_raw,
               s->gx_raw, s->gy_raw, s->gz_raw,
               (unsigned long)s->accel_sq);
    }

    printf("[IMUBuf] end\n");
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
    /* s_seq 不归零，保持序列号单调递增 */
}
