/**
  ******************************************************************************
  * @file    test_aes_sync.c
  * @brief   一致性测试 — 验证两端 AES key/IV 逐字节相同
  *
  * 背景：STM32 用 g_aes128_key/g_aes128_iv 加密广播 payload，
  *       树莓派用 g_key/g_iv 解密。两端若不一致，整条链路静默断裂
  *       （CRC 是加密后算的，解密错误只会得到乱码帧，不易察觉）。
  *
  * 编译（零 glib 依赖，纯 C）：
  *   gcc -Iinclude -I../STM32/Core/User/common \
  *       host_test/test_aes_sync.c src/crypto.c \
  *       "../STM32/Core/User/common/aes128.c" \
  *       -o host_test/test_aes_sync
  *
  * 运行：./host_test/test_aes_sync
  *   PASS → 退出码 0；FAIL → 打印差异字节，退出码 1
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "crypto.h"
#include "aes128.h"

int main(void)
{
    int fail = 0;
    int i;

    /* 1. key 比对 */
    for (i = 0; i < AES_KEY_LEN; i++)
    {
        if (g_aes128_key[i] != g_key[i])
        {
            printf("KEY 不一致 [%2d]: STM32=0x%02X  Pi=0x%02X\n",
                   i, g_aes128_key[i], g_key[i]);
            fail = 1;
        }
    }

    /* 2. IV 比对 */
    for (i = 0; i < AES_IV_LEN; i++)
    {
        if (g_aes128_iv[i] != g_iv[i])
        {
            printf("IV  不一致 [%2d]: STM32=0x%02X  Pi=0x%02X\n",
                   i, g_aes128_iv[i], g_iv[i]);
            fail = 1;
        }
    }

    if (fail)
    {
        printf("\n结果: FAIL — 两端 key/IV 不一致，BLE 链路无法通信\n");
        return 1;
    }

    /* 3. 交叉加解密验证（STM32 加密 → Pi 解密 → 原文） */
    {
        uint8_t plain[] = "ISKBoard-AES-sync-test-0123456789";
        uint8_t buf1[40], buf2[40];
        size_t  len = sizeof(plain);

        memset(buf1, 0, sizeof(buf1));
        memset(buf2, 0, sizeof(buf2));

        aes128_ctr_crypt(plain, buf1, len);   /* Pi 侧加密 */
        AES128_CTR(buf1, buf2, (uint16_t)len); /* STM32 侧解密（CTR 对称） */

        if (memcmp(buf2, plain, len) != 0)
        {
            printf("交叉加解密不一致 — 实现不兼容\n");
            return 1;
        }
    }

    printf("AES key/IV 两端一致，交叉加解密通过\n");
    return 0;
}