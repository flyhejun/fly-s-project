/**
  ******************************************************************************
  * @file    comm_parse.c
  * @brief   Pi 端协议帧解析实现
  *
  * 帧格式（与 STM32 同步）：
  *   SOF(0xAA) | TYPE | LEN(2B LE) | PAYLOAD(LEN 字节) | CRC(XOR) | EOF(0x55)
  ******************************************************************************
  */
#include "comm_parse.h"
#include <string.h>
#include "log.h"
#include "crypto.h"

/* ---- 内部辅助 ---- */

/* 2 字节小端读 */
static uint16_t read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/* 4 字节小端读 */
static uint32_t read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static uint8_t bitrev(uint8_t x)
{
    uint8_t r = 0;
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        r = (uint8_t)((r << 1) | (x & 1));
        x >>= 1;
    }
    return r;
}
/* CRC-8/MAXIM：poly 0x31, init 0x00, xorout 0x00（两端一致） */
static uint8_t calc_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    size_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= bitrev(data[i]);
        for (j = 0; j < 8; j++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return bitrev(crc);
}

/* ---- API ---- */

int comm_parse_frame(const uint8_t *raw, size_t len, ParsedFrame_t *out)
{
    uint16_t payload_len;
    uint8_t  type;
    uint8_t  expected_crc, actual_crc;
    uint8_t  plain[32];     /* 明文缓冲区：解密 payload 用（raw 是 const） */
    const uint8_t *p;

    if (raw == NULL || out == NULL)
        return 0;
    /* 1. 帧长度检查（最短 = SOF+TYPE+LEN+CRC+EOF = 6B） */
    if (len < FRAME_OVERHEAD)
        return 0;
    /* 2. SOF / EOF 检查 */
    if (raw[0] != FRAME_SOF || raw[len - 1] != FRAME_EOF)
        return 0;
    /* 3. 读取 TYPE 和 LEN */
    type        = raw[1];
    payload_len = read_u16(&raw[2]);
    /* 4. 总长应 = SOF+TYPE+LEN+PAYLOAD+CRC+EOF */
    if (len != (size_t)(FRAME_OVERHEAD + payload_len))
        return 0;
    /* 5. CRC8 校验：从 SOF 到 PAYLOAD 密文末尾 */
    expected_crc = raw[4 + payload_len];
    actual_crc   = calc_crc(raw, 4 + payload_len);
    if (expected_crc != actual_crc)
        return 0;

    /* 6. 拷贝到本地缓冲区并解密 payload（CTR 等长，就地解密） */
    memcpy(plain, raw, len);
    if (aes128_ctr_crypt(&plain[4], &plain[4], payload_len) != 0)
        return 0;
    p = &plain[4];

    /* 7. 按类型解析 payload */
    memset(out, 0, sizeof(*out));
    out->type = type;

    /* 日期字段：NOTIFY / REAL_TIME 共用 payload 前 6B，STATUS_REPLY 没有 */
    if (type == FRAME_TYPE_NOTIFY || type == FRAME_TYPE_REAL_TIME)
    {
        out->date.year   = read_u16(&p[0]);
        out->date.month  = p[2];
        out->date.day    = p[3];
        out->date.hour   = p[4];
        out->date.minute = p[5];
    }

    switch (type)
    {
        case FRAME_TYPE_NOTIFY:
            out->data.notify.accel_sq = read_u32(&p[7]);
            out->data.notify.gyro_sq = read_u32(&p[11]);
            break;

        case FRAME_TYPE_REAL_TIME:
            out->data.real_time.accel_sq = read_u32(&p[6]);
            out->data.real_time.gyro_sq  = read_u32(&p[10]);
            break;

        case FRAME_TYPE_STATUS_REPLY:
            out->data.state = p[0];
            break;

        default:
            return 0;
    }

    return 1;
}
