/**
  ******************************************************************************
  * @file    comm_protocol.c
  * @brief   通信协议帧打包实现
  *
  * CRC 算法：从 SOF 到 PAYLOAD 末尾逐字节 XOR。
  *
  * 使用示例：
  *   FallEvent_Data_t event;
  *   IMUBuf_GetPeak(&event);
  *
  *   uint8_t buf[COMM_NOTIFY_FRAME_LEN];
  *   uint16_t len = Comm_PackNotify(buf, &event);
  *   // 通过 UART/BLE 发送 buf, len
  ******************************************************************************
  */
#include "comm_protocol.h"
#include <string.h>
#include <stdio.h>

/* ---- 内部辅助 ---------------------------------------------------------- */

static uint8_t calc_crc(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0;
    uint16_t i;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
    }
    return crc;
}

/* 4 字节小端写入 */
static void write_u32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

/* ---- API ---------------------------------------------------------------- */

/**
  * @brief  打包 EVENT_NOTIFY（即时通知）
  * @param  buf   输出缓冲区，至少 19 字节
  * @param  event 跌倒事件数据
  * @retval 写入字节数（19）
  */
uint16_t Comm_PackNotify(uint8_t *buf, const FallEvent_Data_t *event)
{
    uint16_t idx = 0;

    /* SOF */
    buf[idx++] = COMM_SOF;

    /* TYPE */
    buf[idx++] = COMM_TYPE_NOTIFY;

    /* LEN（小端） */
    buf[idx++] = (uint8_t)(COMM_NOTIFY_PAYLOAD_LEN & 0xFF);
    buf[idx++] = (uint8_t)((COMM_NOTIFY_PAYLOAD_LEN >> 8) & 0xFF);

    /* PAYLOAD：timestamp_ms */
    write_u32(&buf[idx], event->timestamp_ms);
    idx += 4;

    /* PAYLOAD：event_type */
    buf[idx++] = event->event_type;

    /* PAYLOAD：max_accel_sq */
    write_u32(&buf[idx], event->max_accel_sq);
    idx += 4;

    /* PAYLOAD：freefall_min_sq */
    write_u32(&buf[idx], event->freefall_min_sq);
    idx += 4;

    /* CRC：从 SOF 到 PAYLOAD 末尾 */
    buf[idx] = calc_crc(buf, idx);
    idx++;

    /* EOF */
    buf[idx++] = COMM_EOF;

    return idx;
}

/**
  * @brief  打包 EVENT_FULL（完整 256 帧事件）
  *
  * TODO: 由你来实现！
  *
  * @param  buf   输出缓冲区，至少 6150 字节
  * @param  event 跌倒事件数据
  * @retval 写入字节数（6150）
  *
  * 步骤：
  *   1. 写入 SOF (0xAA)
  *   2. 写入 TYPE (0x02)
  *   3. 写入 LEN  (6150 - 6 = 6144，小端)
  *   4. 写入 256 帧 IMU_Sample_t（用 memcpy，或逐字段小端写入）
  *   5. 计算 CRC = XOR(SOF → PAYLOAD 末尾)
  *   6. 写入 EOF (0x55)
  *   7. 返回总字节数
  *
  *   IMU_Sample_t 布局（24 字节）：
  *     timestamp_ms(4) + ax(2)+ay(2)+az(2)+gx(2)+gy(2)+gz(2) + accel_sq(4)+gyro_sq(4)
  *     注意：int16_t 也要转小端！
  */
uint16_t Comm_PackFull(uint8_t *buf, const FallEvent_Data_t *event)
{
    uint16_t idx = 0;
    uint16_t i;

    buf[idx++] = COMM_SOF;
    buf[idx++] = COMM_TYPE_FULL;

    /* LEN（小端） */
    buf[idx++] = (uint8_t)(COMM_FULL_PAYLOAD_LEN & 0xFF);
    buf[idx++] = (uint8_t)((COMM_FULL_PAYLOAD_LEN >> 8) & 0xFF);

    /* PAYLOAD：256 帧 IMU_Sample_t */
    for (i = 0; i < 256; i++) {
        write_u32(&buf[idx], event->samples[i].timestamp_ms);
        idx += 4;

        buf[idx++] = (uint8_t)(event->samples[i].ax_raw & 0xFF);
        buf[idx++] = (uint8_t)((event->samples[i].ax_raw >> 8) & 0xFF);
        buf[idx++] = (uint8_t)(event->samples[i].ay_raw & 0xFF);
        buf[idx++] = (uint8_t)((event->samples[i].ay_raw >> 8) & 0xFF);
        buf[idx++] = (uint8_t)(event->samples[i].az_raw & 0xFF);
        buf[idx++] = (uint8_t)((event->samples[i].az_raw >> 8) & 0xFF);
        buf[idx++] = (uint8_t)(event->samples[i].gx_raw & 0xFF);
        buf[idx++] = (uint8_t)((event->samples[i].gx_raw >> 8) & 0xFF);
        buf[idx++] = (uint8_t)(event->samples[i].gy_raw & 0xFF);
        buf[idx++] = (uint8_t)((event->samples[i].gy_raw >> 8) & 0xFF);
        buf[idx++] = (uint8_t)(event->samples[i].gz_raw & 0xFF);
        buf[idx++] = (uint8_t)((event->samples[i].gz_raw >> 8) & 0xFF);

        write_u32(&buf[idx], event->samples[i].accel_sq);
        idx += 4;

        write_u32(&buf[idx], event->samples[i].gyro_sq);
        idx += 4;
    }

    buf[idx] = calc_crc(buf, idx);
    idx++;
    buf[idx++] = COMM_EOF;

    return idx;
}
