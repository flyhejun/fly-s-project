/**
  ******************************************************************************
  * @file    comm_protocol.c
  * @brief   通信协议帧打包/解析实现
  *
  * CRC 算法：CRC-8/MAXIM (poly 0x31)，从 SOF 到 PAYLOAD 末尾逐字节计算（位反序）。
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
#include "aes128.h"

/* ---- 内部辅助 ---------------------------------------------------------- */

static uint8_t bitrev(uint8_t x)
{
    uint8_t r = 0;
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        r = (uint8_t)(r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

/* CRC-8/MAXIM：poly 0x31, init 0x00, xorout 0x00（两端一致） */
static uint8_t calc_crc(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0;
    uint16_t i, b;

    for (i = 0; i < len; i++)
    {
        crc ^= bitrev(data[i]);
        for (b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return bitrev(crc);
}

/* 4 字节小端写入 */
static void write_u32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

/* 4 字节小端读取 */
static uint32_t read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

/* ---- API ---------------------------------------------------------------- */

/**
  * @brief  打包 EVENT_NOTIFY（即时通知）
  * @param  buf   输出缓冲区，至少 15 字节
  * @param  event 跌倒事件数据
  * @retval 写入字节数（15）
  */
uint16_t Comm_PackNotify(uint8_t *buf, const FallEvent_Data_t *event)
{
    uint16_t idx = 0;

    buf[idx++] = COMM_SOF;
    buf[idx++] = COMM_TYPE_NOTIFY;
    buf[idx++] = (uint8_t)(COMM_NOTIFY_PAYLOAD_LEN & 0xFF);
    buf[idx++] = (uint8_t)((COMM_NOTIFY_PAYLOAD_LEN >> 8) & 0xFF);

    buf[idx++] = event->event_type;

    write_u32(&buf[idx], event->accel_sq);
    idx += 4;

    write_u32(&buf[idx], event->gyro_sq);
    idx += 4;

    AES128_CTR(&buf[4], &buf[4], COMM_NOTIFY_PAYLOAD_LEN);  /* 加密 PAYLOAD */
    /* CRC：从 SOF 到 PAYLOAD 末尾 */
    buf[idx] = calc_crc(buf, idx);
    idx++;

    buf[idx++] = COMM_EOF;

    return idx;
}

/**
  * @brief  打包 REAL_TIME（实时数据）
  * @param  buf      输出缓冲区，至少 14 字节
  * @param  accel_sq 加速度平方和
  * @param  gyro_sq  角速度平方和
  * @retval 写入字节数（14）
  */
uint16_t Comm_PackRealTime(uint8_t *buf, uint32_t accel_sq, uint32_t gyro_sq)
{
    uint16_t idx = 0;

    buf[idx++] = COMM_SOF;
    buf[idx++] = COMM_TYPE_REAL_TIME;
    buf[idx++] = (uint8_t)(COMM_REALTIME_PAYLOAD_LEN & 0xFF);
    buf[idx++] = (uint8_t)((COMM_REALTIME_PAYLOAD_LEN >> 8) & 0xFF);

    write_u32(&buf[idx], accel_sq);
    idx += 4;

    write_u32(&buf[idx], gyro_sq);
    idx += 4;

    AES128_CTR(&buf[4], &buf[4], COMM_REALTIME_PAYLOAD_LEN);  /* 加密 PAYLOAD */
    buf[idx] = calc_crc(buf, idx);
    idx++;

    buf[idx++] = COMM_EOF;

    return idx;
}

/**
  * @brief  打包 STATUS_REPLY（状态查询回复）
  * @param  buf    输出缓冲区，至少 7 字节
  * @param  state  当前跌倒检测状态
  * @retval 写入字节数（7）
  */
uint16_t Comm_PackStatusReply(uint8_t *buf, uint8_t state)
{
    uint16_t idx = 0;

    buf[idx++] = COMM_SOF;
    buf[idx++] = COMM_TYPE_STATUS_REPLY;
    buf[idx++] = (uint8_t)(COMM_STATUS_PAYLOAD_LEN & 0xFF);
    buf[idx++] = (uint8_t)((COMM_STATUS_PAYLOAD_LEN >> 8) & 0xFF);

    buf[idx++] = state;

    AES128_CTR(&buf[4], &buf[4], COMM_STATUS_PAYLOAD_LEN);  /* 加密 PAYLOAD */
    buf[idx] = calc_crc(buf, idx);
    idx++;

    buf[idx++] = COMM_EOF;

    return idx;
}

/**
  * @brief  解析下行指令帧
  * @param  frame  完整帧（含 SOF/EOF）
  * @param  len    帧长
  * @param  cmd    输出：解析结果
  * @retval 1  解析成功
  *         0  帧格式/CRC 错误或类型不支持
  */
int Comm_ParseCmd(const uint8_t *frame, uint16_t len, Comm_Cmd_t *cmd)
{
    uint16_t payload_len;

    /* 最小帧长：SOF+TYPE+LEN+CRC+EOF = 6 */
    if (len < COMM_FRAME_OVERHEAD)
        return 0;
    if (frame[0] != COMM_SOF || frame[len - 1] != COMM_EOF)
        return 0;

    /* 校验 LEN 与实收长度一致 */
    payload_len = frame[2] | ((uint16_t)frame[3] << 8);
    if (len != (uint16_t)(COMM_FRAME_OVERHEAD + payload_len))
        return 0;

    /* CRC8：从 SOF 到 PAYLOAD 末尾 */
    if (calc_crc(frame, 4 + payload_len) != frame[4 + payload_len])
        return 0;

    cmd->type = frame[1];

    switch (cmd->type)
    {
        case COMM_TYPE_SET_THRESHOLD:
            if (payload_len != 5) return 0;
            cmd->param_id = frame[4];
            cmd->value    = read_u32(&frame[5]);
            break;

        case COMM_TYPE_TEST_LED:
        case COMM_TYPE_TEST_BUZZER:
            if (payload_len != 1) return 0;
            cmd->value = frame[4];
            break;

        default:
            return 0;
    }

    return 1;
}