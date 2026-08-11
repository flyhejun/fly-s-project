/**
  ******************************************************************************
  * @file    comm_parse.h
  * @brief   Pi 端协议帧解析 — 对应 STM32 打入的包头
  *
  * 帧格式（与 STM32 一致）：
  *   SOF(0xAA) | TYPE(1B) | LEN(2B LE) | PAYLOAD | CRC(1B XOR) | EOF(0x55)
  *
  * TYPE 常量、字段布局必须跟 STM32 的 comm_protocol.h 同步。
  ******************************************************************************
  */
#ifndef COMM_PARSE_H
#define COMM_PARSE_H

#include <stddef.h>
#include <stdint.h>

/* ---- 帧常量（与 STM32 一致） ---- */
#define FRAME_SOF      0xAA
#define FRAME_EOF      0x55
#define FRAME_OVERHEAD 6

/* ---- 上行帧类型 ---- */
#define FRAME_TYPE_NOTIFY       0x01
#define FRAME_TYPE_REAL_TIME    0x02
#define FRAME_TYPE_STATUS_REPLY 0x03

/* --- 下行帧类型 ---*/
#define FRAME_TYPE_SET_THRESHOLD 0x81
#define FRAME_TYPE_ALARM_CANCEL  0x83
#define FRAME_TYPE_TEST_LED      0x84
#define FRAME_TYPE_TEST_BUZZER   0x85
#define FRAME_TYPE_QUERY_STATUS  0x87

/* Payload 大小 */
#define REAL_TIME_PAYLOAD_LEN   8    /* accel_sq(4)+gyro_sq(4) */
#define NOTIFY_PAYLOAD_LEN      9    /* type(1)+accel_sq(4)+gyro_sq(4) */
#define STATUS_PAYLOAD_LEN      1    /* state(1) */

/* ---- 解析结果结构体 ---- */

/* REAL_TIME 帧的数据字段（不含日期，日期在 ParsedFrame_t.date） */
typedef struct {
    uint32_t accel_sq;
    uint32_t gyro_sq;
} PiRealTime_t;

/* EVENT_NOTIFY 帧的数据字段（不含日期） */
typedef struct {
    uint32_t accel_sq;
    uint32_t gyro_sq;
} PiNotify_t;

/* 解析完成的帧 */
typedef struct {
    uint8_t   type;
    union {
        PiRealTime_t real_time;
        PiNotify_t   notify;
        uint8_t      state;   /* STATUS_REPLY */
    } data;
} ParsedFrame_t;

/* ---- API ---- */

/**
  * @brief  解析一帧原始数据
  * @param  raw     BLE Notify 收到的原始字节
  * @param  len     字节数
  * @param  out     输出：解析结果
  * @retval 1  解析成功（CRC 正确，类型已知）
  *         0  失败（帧格式错误 / CRC 错 / 类型未知）
  */
int comm_parse_frame(const uint8_t *raw, size_t len, ParsedFrame_t *out);

int comm_pack_cmd(uint8_t *buf, size_t buf_size, uint8_t type,
                const uint8_t *payload, size_t payload_len);

#endif /* COMM_PARSE_H */
