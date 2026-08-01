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

#include <stdint.h>

/* ---- 帧常量（与 STM32 一致） ---- */
#define FRAME_SOF      0xAA
#define FRAME_EOF      0x55
#define FRAME_OVERHEAD 6

/* ---- 上行帧类型 ---- */
#define FRAME_TYPE_NOTIFY       0x01
#define FRAME_TYPE_REAL_TIME    0x02
#define FRAME_TYPE_STATUS_REPLY 0x03

/* Payload 大小 */
#define REAL_TIME_PAYLOAD_LEN   14   /* date(6)+accel_sq(4)+gyro_sq(4) */
#define NOTIFY_PAYLOAD_LEN      15   /* date(6)+type(1)+accel_sq(4)+gyro_sq(4) */
#define STATUS_PAYLOAD_LEN      1    /* state(1) */

/* ---- 解析结果结构体 ---- */

/* 日期（年月日时分） */
typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
} PiDate_t;

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
    PiDate_t  date;         /* NOTIFY / REAL_TIME 共用，STATUS_REPLY 不用 */
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

#endif /* COMM_PARSE_H */
