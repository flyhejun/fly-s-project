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
  * 消息类型：
  *   0x01 EVENT_NOTIFY  — 即时通知（13B payload，特征值）
  *   0x02 EVENT_FULL    — 完整事件（6144B payload，256 帧原始数据）
  ******************************************************************************
  */
#ifndef __COMM_PROTOCOL_H
#define __COMM_PROTOCOL_H

#include <stdint.h>
#include "imubuf.h"

/* ================================================================
 *  帧常量
 * ================================================================ */
#define COMM_SOF                0xAA
#define COMM_EOF                0x55
#define COMM_FRAME_OVERHEAD     6       /* SOF+TYPE+LEN+CRC+EOF */

/* 消息类型 */
#define COMM_TYPE_NOTIFY        0x01
#define COMM_TYPE_FULL          0x02

/* Payload 大小 */
#define COMM_NOTIFY_PAYLOAD_LEN 13      /* ts(4)+type(1)+accel(4)+freefall(4) */
#define COMM_FULL_PAYLOAD_LEN   6144    /* 256 × IMU_Sample_t (24B) */

/* 整帧大小 */
#define COMM_NOTIFY_FRAME_LEN   (COMM_FRAME_OVERHEAD + COMM_NOTIFY_PAYLOAD_LEN)  /* 19 */
#define COMM_FULL_FRAME_LEN     (COMM_FRAME_OVERHEAD + COMM_FULL_PAYLOAD_LEN)    /* 6150 */

/* ================================================================
 *  API
 * ================================================================ */
uint16_t Comm_PackNotify(uint8_t *buf, const FallEvent_Data_t *event);
uint16_t Comm_PackFull(uint8_t *buf, const FallEvent_Data_t *event);

#endif /* __COMM_PROTOCOL_H */
