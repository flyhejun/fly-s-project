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

/* ================================================================
 *  变量定义
 * ================================================================ */

static char    rx_line[64];
static uint8_t rx_pos;

volatile uint8_t g_ble_adv_status = 0; /* 原子读写，广播状态 */

volatile uint8_t g_ble_ready = 0;      /* BLE 初始化成功标志 */

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

/* ================================================================
 *  前置声明（内部 static 函数，定义在调用处之后）
 * ================================================================ */
static int  uart2_send(const uint8_t *buf, uint16_t len);
static int  uart2_poll_byte(uint8_t *ch);
static void uart2_flush(void);
static int  send_at_cmd_ex(const char *cmd, uint32_t timeout_ms,
                           char *out, int out_size);
static int  send_at_cmd(const char *cmd, uint32_t timeout_ms);
static void parse_ble_status(const char *line);
static void parse_write_urc(const char *line);
static int  hex_to_bytes(const char *hex, uint8_t *out, uint16_t max_len);

/* ================================================================
 *  BLE 初始化 — 状态机（每步含 OK 验证 + 重试 + 恢复路径）
 * ================================================================ */

/* ---- 状态定义 ----
 * 每次启动先 AT+RESTORE 清空 NVS——实测：NVS 空时完整初始化安全（全 OK），
 * NVS 有旧配置时任何重建操作（BLEINIT/GATT/ADV）都会触发 ESP32 崩溃。
 * 所以先清再初始化，保证总是从干净状态开始。
 */
typedef enum {
    BLE_S_RESTORE = 0,      /* AT+RESTORE 清 NVS */
    BLE_S_WAIT_READY,       /* 循环发 AT 检测 ESP32 ready（重启完成） */
    BLE_S_BLEINIT,          /* AT+BLEINIT=2（必须 OK，长超时不重试） */
    BLE_S_GATTCREATE,       /* AT+BLEGATTSSRVCRE（可 SKIP） */
    BLE_S_GATTSTART,        /* AT+BLEGATTSSRVSTART（可 SKIP） */
    BLE_S_ADVPARAM,         /* AT+BLEADVPARAM=160,160,0,0,7（可 SKIP） */
    BLE_S_ADVSTART,         /* 起广播（必须 OK：带新数据） */
    BLE_S_READY,            /* 初始化完成 */
    BLE_S_FAILED,           /* 初始化失败（等待重试） */
} BLE_State_t;

/* ---- 状态机全局 ---- */
static BLE_State_t g_ble_state       = BLE_S_RESTORE;
static int         g_ble_retry       = 0;
static uint32_t    g_ble_wait_tick   = 0;  /* WAIT_READY 起始时间戳 */

/* AT+BLEADVDATA 运行时错误计数（由 RX ERROR 行触发） */
volatile uint8_t g_ble_advdata_err   = 0;

#define AT_MAX_RETRY  3
#define AT_TIMEOUT_MS 1000
#define BLEINIT_TIMEOUT_MS 5000   /* BLEINIT 初始化 BLE 栈耗时较长 */

/* 每个状态对应的 AT 命令（BLE_S_RESTORE/INIT/READY/FAILED 无命令） */
static const char *state_cmd[] = {
    [BLE_S_BLEINIT]    = "AT+BLEINIT=2",
    [BLE_S_GATTCREATE] = "AT+BLEGATTSSRVCRE",
    [BLE_S_GATTSTART]  = "AT+BLEGATTSSRVSTART",
    [BLE_S_ADVPARAM]   = "AT+BLEADVPARAM=160,160,0,0,7",
    [BLE_S_ADVSTART]   = "AT+BLEADVSTART",
};

/* 每状态超时：GATT 创建/启动、广播启动耗时较长，超时太短会误判失败，
 * 立即发下一条导致 ESP32 处理堆积卡死（手动测试不会因为人在等） */
static const uint32_t state_timeout[] = {
    [BLE_S_BLEINIT]    = 6000,
    [BLE_S_GATTCREATE] = 6000,
    [BLE_S_GATTSTART]  = 6000,
    [BLE_S_ADVPARAM]   = 6000,
    [BLE_S_ADVSTART]   = 6000,
};

/* 允许容错继续的状态：GATTCREATE/GATTSTART/ADVPARAM 返回 ERROR → SKIP 沿用。
 * BLEINIT 和 ADVSTART 不在列表 → 必须 OK。 */
static int state_allow_skip(BLE_State_t s)
{
    switch (s)
    {
        case BLE_S_GATTCREATE:
        case BLE_S_GATTSTART:
        case BLE_S_ADVPARAM:
            return 1;
        default:
            return 0;
    }
}

/**
  * @brief  BLE 初始化状态机 — 每轮推进一步
  *
  * commTask 每 100ms 调用一次，每一步发送一条 AT 命令并等待 OK。
  * 序列：RESTORE → AT → BLEINIT=2 → GATTCREATE → GATTSTART → ADVSTART。
  *  - 先 AT+RESTORE 清 NVS（NVS 空时初始化才安全，有旧配置会崩）
  *  - BLEINIT/ADVSTART 必须 OK；GATTCREATE/GATTSTART/ADVPARAM 可 SKIP
  *  - 失败进 FAILED，30 秒低频重试（ESP32 崩溃后重试无意义，靠断电/RESTORE 恢复）
  */
void ESP32_Init_BLE_Step(void)
{
    int ok;

    /* ---- 就绪 / 失败 态不做事 ---- */
    if (g_ble_state == BLE_S_READY)
    {
        g_ble_ready = 1;
        return;
    }
    if (g_ble_state == BLE_S_FAILED)
    {
        /* ESP32 崩溃后重试无意义（硬件级卡死），低频重试 + 提示 */
        static uint32_t s_fail_tick = 0;
        if (s_fail_tick == 0) s_fail_tick = HAL_GetTick();
        if (HAL_GetTick() - s_fail_tick > 30000)
        {
            s_fail_tick = 0;
            g_ble_state = BLE_S_RESTORE;   /* 30 秒后重来（含 RESTORE） */
            printf("[ESP32] retry (30s)...\n");
        }
        g_ble_ready = 0;
        return;
    }

    /* ---- RESTORE 态：清 NVS，触发 ESP32 重启 ---- */
    if (g_ble_state == BLE_S_RESTORE)
    {
        send_at_cmd("AT+RESTORE", 6000);   /* 回显截断正常——命令触发重启 */
        uart2_flush();                      /* 清重启期间的启动信息 */
        g_ble_wait_tick = HAL_GetTick();
        g_ble_state = BLE_S_WAIT_READY;
        g_ble_ready = 0;
        printf("[ESP32] RESTORE issued, waiting ESP32 ready...\n");
        return;
    }

    /* ---- WAIT_READY：循环发 AT，检测 ESP32 重启完成（不等死固定时间） ---- */
    if (g_ble_state == BLE_S_WAIT_READY)
    {
        if (HAL_GetTick() - g_ble_wait_tick > 30000)
        {
            printf("[ESP32] ESP32 not ready in 30s\n");
            g_ble_state = BLE_S_FAILED;
            g_ble_ready = 0;
            return;
        }
        if (send_at_cmd("AT", AT_TIMEOUT_MS) == 1)
        {
            printf("[ESP32] ESP32 ready after %lums\n",
                   (unsigned long)(HAL_GetTick() - g_ble_wait_tick));
            g_ble_state = BLE_S_BLEINIT;    /* 已确认 ready，直接初始化 */
            g_ble_retry = 0;
            g_ble_ready = 0;
            return;
        }
        HAL_Delay(2000);   /* 还没 ready，等 2 秒再试 */
        g_ble_ready = 0;
        return;
    }

    /* ---- BLEINIT 特殊处理：长超时 + 不重试（栈活着时重复会搞死 ESP32） ---- */
    if (g_ble_state == BLE_S_BLEINIT)
    {
        ok = send_at_cmd("AT+BLEINIT=2", BLEINIT_TIMEOUT_MS);
        if (ok)
        {
            printf("[ESP32] OK: AT+BLEINIT=2\n");
            g_ble_state++;
            g_ble_retry = 0;
        }
        else
        {
            printf("[ESP32] FAIL: AT+BLEINIT=2 -> 请整板断电重启后启动\n");
            g_ble_state = BLE_S_FAILED;
        }
        g_ble_ready = 0;
        return;
    }

    /* ---- 通用命令状态：发 AT 指令，等 OK ---- */
    if (g_ble_state >= BLE_S_READY)
        return;   /* 越界保护：不应走到这里 */

    ok = send_at_cmd(state_cmd[g_ble_state], state_timeout[g_ble_state]);

    if (ok == 1)   /* OK */
    {
        printf("[ESP32] OK: %s\n", state_cmd[g_ble_state]);
        g_ble_retry = 0;
        g_ble_state++;

        if (g_ble_state == BLE_S_READY)
        {
            g_ble_ready = 1;
            printf("[ESP32] BLE initialized successfully\n");
        }
    }
    else if (ok == 0)   /* ERROR：有响应，ESP32 活着 */
    {
        if (state_allow_skip(g_ble_state))
        {
            /* 已配置命令返回 ERROR（如 already set）→ 沿用现有配置 */
            printf("[ESP32] SKIP: %s (not needed, continue)\n", state_cmd[g_ble_state]);
            g_ble_retry = 0;
            g_ble_state++;
        }
        else
        {
            g_ble_retry++;
            printf("[ESP32] FAIL: %s (retry %d/%d)\n",
                   state_cmd[g_ble_state], g_ble_retry, AT_MAX_RETRY);
            if (g_ble_retry >= AT_MAX_RETRY)
                g_ble_state = BLE_S_FAILED;
        }
    }
    else   /* ok == -1：无响应，ESP32 崩溃——重试无意义，直接失败 */
    {
        printf("[ESP32] FAIL: %s (no response, ESP32 down)\n",
               state_cmd[g_ble_state]);
        g_ble_state = BLE_S_FAILED;
    }

    g_ble_ready = 0;
}

/**
  * @brief  重置 BLE 状态机到 INIT，触发重新初始化
  * @note   运行时报错（如 ADVDA 连续失败）时由 commTask 调用。
  *         从 RESTORE（清 NVS）开始重新初始化。
  */
void ESP32_Reset_BLE(void)
{
    g_ble_state     = BLE_S_RESTORE;   /* 重初始化从清 NVS 开始 */
    g_ble_retry     = 0;
    g_ble_ready     = 0;
}

/* ================================================================
 *  上行：数据帧 → ESP32 广播包
 * ================================================================ */

/**
  * @brief  发数据帧（通过 AT+BLEADVDATA 更新广播包）
  *
  * 数据包装格式（BLE AD Structure，31 字节预算内）：
  *   [Flags: 02 01 06] [Name: 02 09 "f"] [Mfg: len FF E5 02] [帧 SOF...EOF]
  *   ── 3B ──────────   ── 3B ─────────   ── 4B ───────────  ── 7~21B ──
  *
  * 短名占位：广播数据自带 Name → ESP32 不再自动加 "Espressif"(13B)，
  * 否则 Name+Mfg 超 31B 上限，Mfg 被截断、Pi 收不到数据。
  * 最长帧 21B + 10B 开销 = 31B，恰好满广播上限。
  * 采用 Fire & Forget 模式（不等待 OK），保证 10Hz 实时数据吞吐。
  */
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

    /* Short Name AD: "Fall" 占位，防 ESP32 自动加 "Espressif" 挤爆 31B */
    ad[ad_len++] = 0x05;   /* 长度 = 1(type) + 4(字符) */
    ad[ad_len++] = 0x09;   /* AD Type: Complete Local Name */
    ad[ad_len++] = 'F';
    ad[ad_len++] = 'a';
    ad[ad_len++] = 'l';
    ad[ad_len++] = 'l';
    /* Manufacturer Data AD（含帧数据） */
    ad[ad_len++] = len + 3;      /* 长度 = 1(type) + 2(mfg_id) + len */
    ad[ad_len++] = 0xFF;         /* AD Type: Manufacturer Specific Data */
    ad[ad_len++] = 0xE5;         /* Company ID low  (0x02E5 = Espressif 官方, 小端) */
    ad[ad_len++] = 0x02;         /* Company ID high */
    memcpy(&ad[ad_len], buf, len);
    ad_len += len;

    /* ---- 2. 生成 AT 命令 ---- */
    hex_pos = strlen("AT+BLEADVDATA=\"");
    memcpy(cmd, "AT+BLEADVDATA=\"", hex_pos);

    for (i = 0; i < ad_len; i++)
    {
        cmd[hex_pos++] = "0123456789ABCDEF"[ad[i] >> 4];
        cmd[hex_pos++] = "0123456789ABCDEF"[ad[i] & 0x0F];
    }
    cmd[hex_pos++] = '\"';
    cmd[hex_pos++] = '\r';
    cmd[hex_pos++] = '\n';

    /* ---- 3. 发送（轮询模式，无中断，直接发） ---- */
    uart2_send((uint8_t *)cmd, hex_pos);
}

/* ================================================================
 *  下行：接收 ESP32 数据 + 重启广播
 * ================================================================ */

/**
  * @brief  UART2 RX 中断逐字喂入
  *
  * 双模式状态机：
  *   行模式  默认，收 +WRITE/BLEDISCONN 状态行，丢弃其余噪声
  *   帧模式  检测到 SOF(0xAA) 进入，按 LEN 计算帧长接收下行指令帧
  */
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
        /* ---- 行模式：收满一行 → 解析 +BLEDISCONN ---- */
        if (ch == '\n' || ch == '\r')
        {
            if (rx_pos > 0)
            {
                rx_line[rx_pos++] = '\0';

                /* 过滤 BLE 连接期间的二进制垃圾（ESP32 发 raw BLE 事件）：
                 * 只处理可打印 ASCII 行，二进制行直接丢弃 */
                {
                    int k;
                    int is_binary = 0;
                    for (k = 0; k < rx_pos - 1; k++)
                    {
                        if ((unsigned char)rx_line[k] < 0x20
                            || (unsigned char)rx_line[k] > 0x7E)
                        {
                            is_binary = 1;
                            break;
                        }
                    }
                    if (!is_binary)
                    {

                /* 诊断：ESP32 的一切非 URC 响应（OK/ERROR/其他） */
                if (strstr(rx_line, "ERROR"))
                {
                    printf("[ESP32 RX] ERROR: %s\n", rx_line);
                    g_ble_advdata_err = 1;  /* 通知 CommTask：ADV 数据更新可能失败了 */
                }
                else if (strstr(rx_line, "OK"))
                    ;   /* OK 静默，避免刷屏 */
                else if (strstr(rx_line, "AT") == NULL
                      && strstr(rx_line, "+WRITE") == NULL
                      && strstr(rx_line, "+BLEDISCONN") == NULL
                      && rx_line[0] != '\0')
                    printf("[ESP32 RX] %s\n", rx_line);

                parse_ble_status(rx_line);
                parse_write_urc(rx_line);   /* +WRITE URC → 下行指令帧 */
                    }  /* !is_binary */
                }
            }
            rx_pos = 0;
        }
        else if (rx_pos < sizeof(rx_line) - 1)
        {
            rx_line[rx_pos++] = (char)ch;
        }
    }
}

static void parse_ble_status(const char *line)
{
    if (strstr(line, "+BLEDISCONN"))
    {
        g_ble_adv_status = 1;
    }
}

static int hex_to_bytes(const char *hex, uint8_t *out, uint16_t max_len)
{
    uint8_t     j;
    uint8_t     i = 0;
    uint8_t     hex_len = strlen(hex);

    if(hex_len % 2 == 1 || (hex_len/2) > max_len)
        return 0;

    while(i < hex_len)
    {
        j = i / 2;

        if(hex[i] >= '0' && hex[i] <= '9')
        {
            if(i % 2 == 0)
            {
                out[j] = ((uint8_t)(hex[i] - '0') << 4);
            }
            else
            {
                out[j] = (((uint8_t)(hex[i] - '0')) | out[j]);    
            }
            
        }
        else if(hex[i] >= 'a' && hex[i] <= 'f')
        {
          if(i % 2 == 0)
          {
            out[j] = ((uint8_t)((hex[i]-'a') + 10) << 4);
          }
          else
          {
            out[j] = ((uint8_t)((hex[i]-'a') + 10) | out[j]);
          }
        }
        else if(hex[i] >= 'A' && hex[i] <= 'F')
        {
          if(i % 2 == 0)
          {
            out[j] = ((uint8_t)((hex[i]-'A') + 10) << 4);
          }
          else
          {
            out[j] = ((uint8_t)((hex[i]-'A') + 10) | out[j]);
          }
        }
        else
            return 0;

        i++;   
    }
    return (int)(hex_len/2);
         
}

/** hex 字符 → 半字节值（非法字符返回 0xFF） */
static uint8_t hexval(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0xFF;
}

/**
  * @brief  解析 ESP32 GATTS 的 +WRITE URC，提取下行指令帧
  * @note   BLE 客户端写特征值时，ESP-AT 经 UART 上报 +WRITE 行。
  *         本函数兼容 value 的两种文本格式（连续 hex / <0xHH> 序列）；
  *         原始字节场景由 0xAA 帧头触发逻辑处理，不会走到这里。
  *         入队前只验 SOF/EOF，CRC 由 Comm_ParseCmd 再验。
  */
static void parse_write_urc(const char *line)
{
    const char *p;
    const char *value;
    uint8_t     frame[sizeof(rx_frame)];
    uint16_t    flen;
    int         n;

    if (strstr(line, "+WRITE") == NULL)
        return;

    /* ---- 尖括号格式 <0xHH>,<0xHH>,...：value 本身按逗号分隔，
     *      不能"取最后一个字段"，改为整行扫描 <0xHH> 模式（header 无 '<'） ---- */
    flen = 0;
    if (strstr(line, "<0x"))
    {
        for (p = line; *p != '\0' && flen < sizeof(frame); )
        {
            if (p[0] == '<' && p[1] == '0' && (p[2] == 'x' || p[2] == 'X')
                && p[3] != '\0' && p[4] != '\0' && p[5] == '>')
            {
                uint8_t hi = hexval(p[3]);
                uint8_t lo = hexval(p[4]);
                if (hi <= 0x0F && lo <= 0x0F)
                {
                    frame[flen++] = (uint8_t)((hi << 4) | lo);
                    p += 6;
                    continue;
                }
            }
            p++;
        }
    }
    else
    {
        /* ---- 连续 hex 字符串：取最后一个 ',' 或 ':' 之后的字段 ---- */
        p = strrchr(line, ',');
        value = strrchr(line, ':');
        if (p != NULL && (value == NULL || p > value))
            value = p + 1;
        else if (value != NULL)
            value = value + 1;
        else
            return;

        n = hex_to_bytes(value, frame, sizeof(frame));
        if (n <= 0 || (uint16_t)n > sizeof(frame))
            return;
        flen = (uint16_t)n;
    }

    /* ---- 校验 SOF/EOF，合法才入队（CRC 由 Comm_ParseCmd 再验） ---- */
    if (flen < COMM_FRAME_OVERHEAD)
        return;
    if (frame[0] != COMM_SOF || frame[flen - 1] != COMM_EOF)
        return;

    memcpy(rx_frame_ring[rx_frame_head], frame, flen);
    rx_frame_ring_len[rx_frame_head] = flen;
    rx_frame_head = (uint8_t)((rx_frame_head + 1) % RX_FRAME_RING_SIZE);
    if (rx_frame_head == rx_frame_tail)
        rx_frame_tail = (uint8_t)((rx_frame_tail + 1) % RX_FRAME_RING_SIZE);
}

/**
  * @brief  取回完整下行指令帧
  * @param  buf  输出：帧数据（含 SOF/EOF）
  * @param  len  输出：帧长
  * @retval 1  取到一帧（buf 已填充）
  *         0  无完整帧
  * @note   commTask 轮询调用；取走后清除就绪标志
  */
uint8_t ESP32_RX_GetFrame(uint8_t *buf, uint16_t *len)
{
    if (rx_frame_tail == rx_frame_head)
        return 0;   /* 无待取帧 */

    memcpy(buf, rx_frame_ring[rx_frame_tail], rx_frame_ring_len[rx_frame_tail]);
    *len = rx_frame_ring_len[rx_frame_tail];

    rx_frame_tail = (uint8_t)((rx_frame_tail + 1) % RX_FRAME_RING_SIZE);
    return 1;
}

/**
  * @brief  轮询接收 USART2 所有待收字节
  * @note   commTask 周期调用。取代中断接收，结构性避免中断风暴：
  *         无论 ESP32 发多少数据，只在调用时读一次，不会占满 CPU。
  */
void ESP32_RX_Poll(void)
{
    uint8_t ch;

    while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE))
    {
        ch = (uint8_t)(huart2.Instance->RDR & 0xFF);
        ESP32_RX_Char(ch);
    }
    /* 清除溢出错误（数据过快时可能置位） */
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE))
    {
        __HAL_UART_CLEAR_OREFLAG(&huart2);
    }
}

/**
  * @brief  检测断开后重启广播
  *
  * 在中断里检测到 +BLEDISCONN 只置了 g_ble_adv_restart 标志。
  * 本函数在 commTask（任务上下文）轮询调用，发现标志后补发
  * AT+BLEADVSTART，恢复可连接广播。
  *
  * 注意：必须放任务里，不能放中断——HAL_UART_Transmit 是阻塞的。
  */
/**
  * @brief  断开后恢复广播（仅限 +BLEDISCONN URC 触发）
  *
  * ESP32 AT 固件在断开后会自动恢复广播，一般不需要手动重启。
  * 此函数仅处理 URC 明确到达的情况，不做主动定时重启
  * （主动重启会清空 ADV 数据，反而导致 Pi 端收不到帧）。
  */
void ESP32_CheckAdvStatus(void)
{
    if (g_ble_adv_status)
    {
        g_ble_adv_status = 0;

        /* fire-and-forget：不等响应，广播已运行时 ESP32 静默忽略 */
        uart2_send((const uint8_t *)"AT+BLEADVSTART\r\n", 17);
    }
}

/* ================================================================
 *  内部工具：UART 读写
 * ================================================================ */

/* 统一发送入口：固定 100ms 超时 */
static int uart2_send(const uint8_t *buf, uint16_t len)
{
    return HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, 100);
}

/**
  * @brief  非阻塞读取一个字节（有数据立即返回，不等）
  * @param  ch  输出：读取到的字节
  * @retval 1  成功
  *         0  暂无数据
  * @note   每次读都清 ORE——ESP32 重启瞬间 UART 引脚噪声会触发 ORE，
  *         不清则 USART 接收卡死，后续命令全无响应。
  */
static int uart2_poll_byte(uint8_t *ch)
{
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE))
    {
        *ch = (uint8_t)(huart2.Instance->RDR & 0xFF);
        if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE))
            __HAL_UART_CLEAR_OREFLAG(&huart2);
        return 1;
    }
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_ORE))
        __HAL_UART_CLEAR_OREFLAG(&huart2);
    return 0;
}

/** 丢弃 UART 接收缓冲区中的残留数据（如 ESP32 启动信息） */
static void uart2_flush(void)
{
    uint8_t dummy;
    while (uart2_poll_byte(&dummy))   /* 快速排空，不等待 */
        ;
}

/**
  * @brief  发送 AT 命令并等待响应（可取出完整响应内容）
  * @param  cmd         AT 命令字符串（不含 \r\n）
  * @param  timeout_ms  单条响应等待时间（毫秒）
  * @param  out         输出：完整响应文本（可为 NULL）
  * @param  out_size    out 缓冲区大小
  * @retval 1   收到 OK
  *         0   有响应但非 OK（ERROR 等）
  *         -1  无响应（ESP32 崩溃/未 ready）
  */
static int send_at_cmd_ex(const char *cmd, uint32_t timeout_ms,
                          char *out, int out_size)
{
    uint8_t  ch;
    uint32_t start;
    char     buf[256];      /* 响应收集缓冲 */
    char     txbuf[128];    /* 发送缓冲（命令+\r\n 一次发出） */
    int      pos = 0;
    int      tx_hal;
    int      cmd_len;

    cmd_len = strlen(cmd);

    /* 发送命令：命令 + \r\n 一次发出，避免两次发送之间的间隙被 ESP32 误处理 */
    if (cmd_len + 2 < (int)sizeof(txbuf))
    {
        memcpy(txbuf, cmd, cmd_len);
        txbuf[cmd_len]     = '\r';
        txbuf[cmd_len + 1] = '\n';
        tx_hal = uart2_send((const uint8_t *)txbuf, cmd_len + 2);
    }
    else
    {
        tx_hal = -1;   /* 命令过长 */
    }

    /* 收集响应：ESP 有回复（OK/ERROR）就判断；无回复则等满超时兜底 */
    memset(buf, 0, sizeof(buf));
    start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        if (uart2_poll_byte(&ch))
        {
            if (pos < (int)sizeof(buf) - 1)
                buf[pos++] = (char)ch;

            if (strstr(buf, "OK") != NULL || strstr(buf, "ERROR") != NULL)
                break;   /* ESP 已回复，判断 */
        }
    }

    /* 判断响应：OK / ERROR（有响应） / none（无响应） */
    if (strstr(buf, "OK") != NULL)
    {
        if (out && out_size > 0)
        {
            strncpy(out, buf, out_size - 1);
            out[out_size - 1] = '\0';
        }
        return 1;   /* OK */
    }

    if (pos > 0)
    {
        printf("[UART2] TX=%d len=%d wait=%lums/%lums resp=%d bytes: %s\n",
               tx_hal, cmd_len, (unsigned long)(HAL_GetTick() - start),
               (unsigned long)timeout_ms, pos, buf);
        return 0;   /* 有响应但非 OK（ERROR） */
    }

    printf("[UART2] TX=%d len=%d wait=%lums/%lums resp=0 (no response)\n",
           tx_hal, cmd_len, (unsigned long)(HAL_GetTick() - start),
           (unsigned long)timeout_ms);
    return -1;      /* 无响应 */
}

static int send_at_cmd(const char *cmd, uint32_t timeout_ms)
{
    return send_at_cmd_ex(cmd, timeout_ms, NULL, 0);
}