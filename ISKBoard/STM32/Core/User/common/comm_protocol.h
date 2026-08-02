/**
  ******************************************************************************
  * @file    comm_protocol.h
  * @brief   STM32 → 树莓派 通信协议帧格式
  *
  * 帧结构（总开销 6 字节）：
  *   ┌──────┬──────┬──────────┬──────────┬──────┬──────┐
  *   │ SOF  │ TYPE │   LEN    │ PAYLOAD  │ CRC  │ EOF  │
  *   │ 1B   │ 1B   │ 2B (LE)  │ LEN 字节  │ 1B   │ 1B   │
  *   │0xAA  │      │          │          │ XOR  │0x55  │
  *   └──────┴──────┴──────────┴──────────┴──────┴──────┘
  *
  * 消息类型（上行 STM32→上位机）：
  *   0x01 EVENT_NOTIFY  — 跌倒事件通知（15B payload，date(6)+type+accel+gyro）
  *   0x02 REAL_TIME     — 实时 accel_sq/gyro_sq（14B payload，date(6)+accel+gyro，10Hz）
  *   0x03 STATUS_REPLY  — 状态查询回复（1B payload：当前状态机状态）
  *
  * 消息类型（下行 上位机→STM32）：
  *   0x81 SET_THRESHOLD — 修改阈值（param_id + value，5B payload）
  *   0x83 ALARM_CANCEL  — 取消报警（0B payload）
  *   0x84 TEST_LED      — 测试 LED（on/off，1B payload）
  *   0x85 TEST_BUZZER   — 测试蜂鸣器（on/off，1B payload）
  *   0x86 TIME_SYNC     — 时间同步（6B payload：year(2)+month+day+hour+minute，
  *                        上位机周期性下发，STM32 直接存储不换算）
  *   0x87 QUERY_STATUS  — 查询状态（0B payload）
  ******************************************************************************
  */
#ifndef __COMM_PROTOCOL_H
#define __COMM_PROTOCOL_H

#include <stdint.h>

/* ================================================================
 *  日期时间（年月日时分，上位机 TIME_SYNC 周期性同步）
 * ================================================================ */
typedef struct {
    uint16_t year;      /* 如 2026 */
    uint8_t  month;     /* 1-12 */
    uint8_t  day;       /* 1-31 */
    uint8_t  hour;      /* 0-23 */
    uint8_t  minute;    /* 0-59 */
} Comm_DateTime_t;

/* ================================================================
 *  统一跌倒事件
 * ================================================================ */
typedef struct {
    Comm_DateTime_t date;   /* 最近一次同步的年月日时分 */
    uint8_t         event_type;
    uint32_t        accel_sq;
    uint32_t        gyro_sq;
} FallEvent_Data_t;

/* ================================================================
 *  帧常量
 * ================================================================ */
#define COMM_SOF                0xAA
#define COMM_EOF                0x55
#define COMM_FRAME_OVERHEAD     6       /* SOF+TYPE+LEN+CRC+EOF */

/* ---- 上行消息类型 ---- */
#define COMM_TYPE_NOTIFY        0x01
#define COMM_TYPE_REAL_TIME     0x02
#define COMM_TYPE_STATUS_REPLY  0x03

/* ---- 下行指令类型 ---- */
#define COMM_TYPE_SET_THRESHOLD 0x81
#define COMM_TYPE_ALARM_CANCEL  0x83
#define COMM_TYPE_TEST_LED      0x84
#define COMM_TYPE_TEST_BUZZER   0x85
#define COMM_TYPE_TIME_SYNC     0x86
#define COMM_TYPE_CHECK_STATUS  0x87

/* ---- 阈值参数 ID（SET_THRESHOLD，仅阈值可改，gyro 不可下发） ---- */
#define COMM_PARAM_FREEFALL_THRESHOLD  0x01
#define COMM_PARAM_IMPACT_THRESHOLD    0x02
#define COMM_PARAM_STILL_LOW           0x03
#define COMM_PARAM_STILL_HIGH          0x04

/* ---- Payload 大小 ---- */
#define COMM_NOTIFY_PAYLOAD_LEN     15      /* date(6)+type(1)+accel_sq(4)+gyro_sq(4) */
#define COMM_REALTIME_PAYLOAD_LEN   14      /* date(6)+accel_sq(4)+gyro_sq(4)         */
#define COMM_TIME_SYNC_PAYLOAD_LEN  6       /* year(2)+month+day+hour+minute          */
#define COMM_STATUS_PAYLOAD_LEN     1       /* state(1)                               */

/* ---- 整帧大小 ---- */
#define COMM_NOTIFY_FRAME_LEN      (COMM_FRAME_OVERHEAD + COMM_NOTIFY_PAYLOAD_LEN)
#define COMM_REALTIME_FRAME_LEN    (COMM_FRAME_OVERHEAD + COMM_REALTIME_PAYLOAD_LEN)
#define COMM_STATUS_FRAME_LEN      (COMM_FRAME_OVERHEAD + COMM_STATUS_PAYLOAD_LEN)

/* ================================================================
 *  下行指令解析结果
 * ================================================================ */
typedef struct {
    uint8_t         type;       /* 指令类型 */
    uint8_t         param_id;   /* SET_THRESHOLD 的参数 ID */
    uint32_t        value;      /* 参数值 / on-off */
    Comm_DateTime_t date;       /* TIME_SYNC 的年月日时分 */
} Comm_Cmd_t;

/* ================================================================
 *  API
 * ================================================================ */
uint16_t Comm_PackNotify(uint8_t *buf, const FallEvent_Data_t *event);

uint16_t Comm_PackRealTime(uint8_t *buf, const Comm_DateTime_t *date,
                           uint32_t accel_sq, uint32_t gyro_sq);

uint16_t Comm_PackStatusReply(uint8_t *buf, uint8_t state);

/**
  * @brief  解析下行指令帧
  * @param  frame  完整帧（含 SOF/EOF）
  * @param  len    帧长
  * @param  cmd    输出：解析结果
  * @retval 1  解析成功
  *         0  帧格式/CRC 错误或类型不支持
  */
int Comm_ParseCmd(const uint8_t *frame, uint16_t len, Comm_Cmd_t *cmd);

#endif /* __COMM_PROTOCOL_H */