/**
  ******************************************************************************
  * @file    esp32_uart.c
  * @brief   ESP32 UART 驱动实现
  ******************************************************************************
  */
#include "esp32_uart.h"
#include "comm_protocol.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

static char    rx_line[64];
static uint8_t rx_pos;

/* ---- 下行指令帧接收（中断上下文） ---- */
#define RX_FRAME_RING_SIZE  4       /* 帧环形缓冲深度 */

static uint8_t  rx_frame[64];       /* 收帧暂存 */
static uint16_t rx_frame_pos;       /* 已收字节数 */
static uint16_t rx_expect_len;      /* 期望总帧长 = 6 + payload_len */
static uint8_t  rx_frame_mode;      /* 1=帧模式 */

/* 帧环形缓冲（SPSC：中断写 head，任务读 tail，无锁） */
static uint8_t  rx_frame_ring[RX_FRAME_RING_SIZE][64];
static uint16_t rx_frame_ring_len[RX_FRAME_RING_SIZE];
static volatile uint8_t rx_frame_head;      /* 写索引（中断） */
static volatile uint8_t rx_frame_tail;      /* 读索引（任务） */

/* 统一发送入口：固定 100ms 超时 */
static int uart2_send(const uint8_t *buf, uint16_t len)
{
    return HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, 100);
}

/* ================================================================
 *  ESP32_Send — 发数据帧（通过 AT+BLEADVDATA 更新广播包）
 *
 *  数据包装格式（BLE AD Structure）：
 *    [Flags: 02 01 06] [MfgData: len FF FF FF] [帧数据 SOF...EOF]
 *    ── 3B ──────────   ── 4B ──────────────   ── 7~21B ─────
 *
 *  最长帧 21B + 7B 开销 = 28B，HEX 编码后 56 字符，符合 31 字节限制。
 *  采用 Fire & Forget 模式（不等待 OK），保证 10Hz 实时数据吞吐。
 * ================================================================ */
void ESP32_Send(const uint8_t *buf, uint16_t len)
{
    uint8_t ad[64];         /* AD Structure 构建缓冲 */
    uint8_t ad_len = 0;
    char    cmd[128];       /* AT+BLEADVDATA="hex..."\r\n */
    int     hex_pos;
    int     i;

    /* ---- 1. 构建 BLE AD Structure ---- */

    /* Flags AD: LE General Discoverable, BR/EDR not supported */
    ad[ad_len++] = 0x02;   /* 长度 */
    ad[ad_len++] = 0x01;   /* AD Type: Flags */
    ad[ad_len++] = 0x06;   /* LE General Discoverable */

    /* Manufacturer Data AD（含帧数据） */
    ad[ad_len++] = len + 3;      /* 长度 = 1(type) + 2(mfg_id) + len */
    ad[ad_len++] = 0xFF;         /* AD Type: Manufacturer Specific Data */
    ad[ad_len++] = 0xFF;         /* Company ID low  (0xFFFF = 测试用) */
    ad[ad_len++] = 0xFF;         /* Company ID high */
    memcpy(&ad[ad_len], buf, len);
    ad_len += len;

    /* ---- 2. 生成 AT 命令 ---- */
    memcpy(cmd, "AT+BLEADVDATA=\"", 16);
    hex_pos = 16;

    for (i = 0; i < ad_len; i++)
    {
        cmd[hex_pos++] = "0123456789ABCDEF"[ad[i] >> 4];
        cmd[hex_pos++] = "0123456789ABCDEF"[ad[i] & 0x0F];
    }
    cmd[hex_pos++] = '\"';
    cmd[hex_pos++] = '\r';
    cmd[hex_pos++] = '\n';

    /* ---- 3. 发送（Fire & Forget） ---- */
    uart2_send((uint8_t *)cmd, hex_pos);
}

/* ================================================================
 *  ESP32_RX_Char — UART2 RX 中断逐字喂入
 *
 *  双模式状态机：
 *    行模式  默认，丢弃非帧数据（ESP32 启动信息 / AT 响应）
 *    帧模式  检测到 SOF(0xAA) 进入，按 LEN 计算帧长接收下行指令帧
 * ================================================================ */
void ESP32_RX_Char(uint8_t ch)
{
    uint16_t plen;

    if (rx_frame_mode)
    {
        /* ---- 帧模式：收下行指令帧 ---- */
        if (rx_frame_pos < sizeof(rx_frame))
        {
            rx_frame[rx_frame_pos++] = ch;

            /* 收满帧头（SOF+TYPE+LEN）后计算期望总长 */
            if (rx_frame_pos == 4)
            {
                plen = rx_frame[2] | ((uint16_t)rx_frame[3] << 8);
                rx_expect_len = COMM_FRAME_OVERHEAD + plen;
                if (rx_expect_len > sizeof(rx_frame))
                    rx_frame_mode = 0;   /* 超长帧，丢弃回行模式 */
            }
            else if (rx_expect_len > 0 && rx_frame_pos >= rx_expect_len)
            {
                /* 收满整帧：末字节应为 EOF，CRC 由 commTask 再校验 */
                if (rx_frame[rx_expect_len - 1] == COMM_EOF)
                {
                    /* 写入环形缓冲，满则丢弃最旧帧 */
                    memcpy(rx_frame_ring[rx_frame_head], rx_frame, rx_frame_pos);
                    rx_frame_ring_len[rx_frame_head] = rx_frame_pos;
                    rx_frame_head = (uint8_t)((rx_frame_head + 1) % RX_FRAME_RING_SIZE);
                    if (rx_frame_head == rx_frame_tail)
                        rx_frame_tail = (uint8_t)((rx_frame_tail + 1) % RX_FRAME_RING_SIZE);
                }
                rx_frame_mode = 0;       /* 收帧结束，回行模式 */
            }
        }
        else
        {
            rx_frame_mode = 0;           /* 缓冲溢出，丢弃 */
        }
    }
    else if (ch == COMM_SOF)
    {
        /* ---- 检测到帧头 → 切帧模式 ---- */
        rx_frame_mode  = 1;
        rx_frame_pos   = 0;
        rx_expect_len  = 0;
        rx_frame[rx_frame_pos++] = ch;
    }
    else
    {
        /* ---- 行模式：丢弃非帧数据（ESP32 启动信息等） ---- */
        if (ch == '\n' || ch == '\r')
        {
            rx_pos = 0;
        }
        else if (rx_pos < sizeof(rx_line) - 1)
        {
            rx_line[rx_pos++] = (char)ch;
        }
    }
}

/* ================================================================
 *  ESP32_RX_GetFrame — 取回完整下行指令帧
 *  commTask 轮询调用；取走后清除就绪标志
 * ================================================================ */
uint8_t ESP32_RX_GetFrame(uint8_t *buf, uint16_t *len)
{
    if (rx_frame_tail == rx_frame_head)
        return 0;   /* 无待取帧 */

    memcpy(buf, rx_frame_ring[rx_frame_tail], rx_frame_ring_len[rx_frame_tail]);
    *len = rx_frame_ring_len[rx_frame_tail];

    rx_frame_tail = (uint8_t)((rx_frame_tail + 1) % RX_FRAME_RING_SIZE);
    return 1;
}

/* ================================================================
 *  BLE 初始化 — AT 命令轮询模式
 *
 * 设计说明：
 *   - 初始化期间临时暂停 UART2 中断接收，改用直接读 RDR 寄存器轮询
 *   - 避免与 HAL_UART_Receive_IT 的状态机冲突（RxState = BUSY_RX 时
 *     HAL_UART_Receive 会拒绝服务）
 *   - 初始化完成后恢复中断模式，不影响后续正常收发
 * ================================================================ */

/* ---- 内部工具：UART 轮询读 ---- */

/** 暂停 UART2 中断接收 */
static void uart2_pause_it(void)
{
    HAL_NVIC_DisableIRQ(USART2_IRQn);
}

/** 恢复 UART2 中断接收 */
static void uart2_resume_it(void)
{
    /* 重启中断接收（指向 usart.c 中的 rx_byte） */
    HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/**
  * @brief  非阻塞读取一个字节（有数据立即返回，不等）
  * @param  ch  输出：读取到的字节
  * @retval 1  成功
  *         0  暂无数据
  */
static int uart2_poll_byte(uint8_t *ch)
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE))
    {
        *ch = (uint8_t)(huart2.Instance->RDR & 0xFF);
        return 1;
    }
    return 0;
}

/**
  * @brief  轮询读取一个字节，带超时
  * @param  ch          输出：读取到的字节
  * @param  timeout_ms  超时时间（毫秒）
  * @retval 1  成功
  *         0  超时
  */
static int uart2_read_byte(uint8_t *ch, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        if (uart2_poll_byte(ch))
            return 1;
    }
    return 0;
}

/** 丢弃 UART 接收缓冲区中的残留数据（如 ESP32 启动信息） */
static void uart2_flush(void)
{
    uint8_t dummy;
    while (uart2_poll_byte(&dummy))   /* 快速排空，不等待 */
        ;
}

/* ---- AT 命令交互 ---- */

/**
  * @brief  发送 AT 命令并等待 OK/ERROR 响应
  * @param  cmd         AT 命令字符串（不含 \r\n）
  * @param  timeout_ms  单条响应等待时间（毫秒）
  * @retval 1  收到 OK
  *         0  超时或收到 ERROR
  */
static int send_at_cmd(const char *cmd, uint32_t timeout_ms)
{
    char    resp[64];   /* 每次读取一行 */
    int     pos;
    uint8_t ch;
    uint32_t start;
    int     found_ok = 0;

    /* --- 发送命令 --- */
    uart2_send((const uint8_t *)cmd, strlen(cmd));
    uart2_send((const uint8_t *)"\r\n", 2);

    /* --- 逐行读取响应 --- */
    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        /* 读取一行（遇 \n 结束） */
        pos = 0;
        memset(resp, 0, sizeof(resp));

        while (pos < (int)sizeof(resp) - 1)
        {
            if (!uart2_read_byte(&ch, timeout_ms - (HAL_GetTick() - start)))
            {
                goto done;   /* 超时 */
            }

            if (ch == '\n')
            {
                break;       /* 行结束 */
            }
            if (ch != '\r')
            {
                resp[pos++] = (char)ch;
            }
        }

        /* --- 检查行内容 --- */
        if (strstr(resp, "OK") != NULL)
        {
            found_ok = 1;
            goto done;
        }
        if (strstr(resp, "ERROR") != NULL)
        {
            goto done;       /* 失败 */
        }
    }

done:
    return found_ok;
}

/* ---- AT 命令序列 ---- */

/* BLE 初始化 AT 命令链表（广播模式，无需 GATT） */
static const char *at_sequence[] = {
    "AT",                        /* 测试 ESP32 是否在线       */
    "AT+BLEINIT=2",              /* 初始化为 BLE Server       */
    "AT+BLENAME=FallSensor",     /* 设置 BLE 广播名           */
    "AT+BLEADVPARAM=160,160,0,0,7",  /* 广播参数（100ms间隔）*/
    "AT+BLEADVSTART",            /* 开始 BLE 广播             */
};

#define AT_CMD_COUNT  (sizeof(at_sequence) / sizeof(at_sequence[0]))
#define AT_MAX_RETRY  3
#define AT_TIMEOUT_MS 1000

/**
  * @brief  ESP32 BLE 初始化（阻塞）
  *
  * 执行流程：
  *   1. 暂停 UART2 中断接收 → 切换轮询模式
  *   2. 清空 ESP32 可能发出的启动信息
  *   3. 依次发送 AT 命令，每条最多重试 3 次
  *   4. 恢复 UART2 中断接收
  *
  * @note  应在 RTOS 任务中调用（依赖 osDelay/HAL_GetTick）
  */
BLE_Init_Status_t ESP32_Init_BLE(void)
{
    int i, retry;
    int ok;

    /* 1. 切换为轮询模式 */
    uart2_pause_it();

    /* 2. 清空启动垃圾信息 */
    uart2_flush();

    /* 3. 执行 AT 命令序列 */
    for (i = 0; i < (int)AT_CMD_COUNT; i++)
    {
        ok = 0;
        for (retry = 0; retry < AT_MAX_RETRY; retry++)
        {
            ok = send_at_cmd(at_sequence[i], AT_TIMEOUT_MS);
            if (ok) break;
        }

        if (!ok)
        {
            printf("[ESP32] AT FAIL: %s\n", at_sequence[i]);
            uart2_resume_it();
            return BLE_INIT_FAIL;
        }
        printf("[ESP32] AT OK: %s\n", at_sequence[i]);
    }

    /* 4. 恢复中断模式 */
    uart2_resume_it();

    printf("[ESP32] BLE initialized successfully\n");
    return BLE_INIT_OK;
}
