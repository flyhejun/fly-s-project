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
  *   0x01 EVENT_NOTIFY  — 跌倒事件通知（9B payload：type+accel_sq+gyro_sq）
  *   0x02 REAL_TIME     — 实时 accel_sq/gyro_sq（8B payload，10Hz）
  *   0x03 STATUS_REPLY  — 状态查询回复（1B payload：当前状态机状态）
  *
  * 消息类型（下行 上位机→STM32）：
  *   0x81 SET_THRESHOLD — 修改阈值（param_id + value，5B payload）
  *   0x84 TEST_LED      — 测试 LED（on/off，1B payload）
 *   （ALARM_CANCEL/QUERY_STATUS 已移除：报警取消走板子按键，状态走周期自动上报）
  *   0x85 TEST_BUZZER   — 测试蜂鸣器（on/off，1B payload）
  *
  * 说明：上行帧不携带时间，由树莓派接收时打本地时间戳。
  ******************************************************************************
  */
#ifndef __COMM_PROTOCOL_H
#define __COMM_PROTOCOL_H

#include <stdint.h>

/* ================================================================
 *  统一跌倒事件
 * ================================================================ */
typedef struct {
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
#define COMM_TYPE_TEST_LED      0x84
#define COMM_TYPE_TEST_BUZZER   0x85

/* ---- 阈值参数 ID（SET_THRESHOLD，仅阈值可改，gyro 不可下发） ---- */
#define COMM_PARAM_FREEFALL_THRESHOLD  0x01
#define COMM_PARAM_IMPACT_THRESHOLD    0x02
#define COMM_PARAM_STILL_LOW           0x03
#define COMM_PARAM_STILL_HIGH          0x04

/* ---- Payload 大小 ---- */
#define COMM_NOTIFY_PAYLOAD_LEN     9       /* type(1)+accel_sq(4)+gyro_sq(4) */
#define COMM_REALTIME_PAYLOAD_LEN   8       /* accel_sq(4)+gyro_sq(4)         */
#define COMM_STATUS_PAYLOAD_LEN     1       /* state(1)                       */

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
} Comm_Cmd_t;

/* ================================================================
 *  API
 * ================================================================ */
uint16_t Comm_PackNotify(uint8_t *buf, const FallEvent_Data_t *event);

uint16_t Comm_PackRealTime(uint8_t *buf, uint32_t accel_sq, uint32_t gyro_sq);

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