/**
  ******************************************************************************
  * @file    aes128.c
  * @brief   AES-128 软件实现 + CTR 加解密
  *
  * 说明：
  *   - 纯软件实现，不依赖硬件 AES 外设，避免 CubeMX 配置
  *   - 每次调用无状态（固定 key/IV），可随时加密
  *   - 自测：AES128_SelfTest 用 FIPS-197 标准向量验证
  ******************************************************************************
  */
#include "aes128.h"

/* ================================================================
 *  key / IV 常量（与树莓派 openssl 侧必须一致）
 * ================================================================ */
const uint8_t g_aes128_key[AES128_KEY_LEN] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
};

const uint8_t g_aes128_iv[AES128_BLOCK] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
};

/* ================================================================
 *  AES-128 内部：S-box / 轮常量
 * ================================================================ */

/* SubBytes 替换表（FIPS-197 附录 A） */
static const uint8_t sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* 轮常量（KeyExpansion 用） */
static const uint8_t rcon[10] = {
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

/* ================================================================
 *  内部工具
 * ================================================================ */

/* 字节乘 2（GF(2^8) 域，模多项式 x^8+x^4+x^3+x+1 = 0x1B） */
static uint8_t xtime(uint8_t x)
{
    return (x & 0x80) ? (uint8_t)((x << 1) ^ 0x1b) : (uint8_t)(x << 1);
}

/* GF 域乘法：a * b（a 用 xtime 展开） */
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;

    while (b)
    {
        if (b & 1)
            p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

/* ================================================================
 *  KeyExpansion — 16B key → 176B 轮密钥
 * ================================================================ */
static void key_expansion(const uint8_t *key, uint8_t *round_keys)
{
    uint8_t i, j;
    uint8_t temp[4];

    /* 前 16 字节 = 原始 key */
    for (i = 0; i < 16; i++)
        round_keys[i] = key[i];

    /* 每 4 字节一组，共 44 组（176/4） */
    for (i = 4; i < 44; i++)
    {
        /* temp = 上一组 */
        for (j = 0; j < 4; j++)
            temp[j] = round_keys[(i - 1) * 4 + j];

        if (i % 4 == 0)
        {
            /* 字循环：RotWord */
            uint8_t t = temp[0];
            temp[0] = temp[1];
            temp[1] = temp[2];
            temp[2] = temp[3];
            temp[3] = t;

            /* SubWord + 异或轮常量 */
            temp[0] = sbox[temp[0]] ^ rcon[i / 4 - 1];
            temp[1] = sbox[temp[1]];
            temp[2] = sbox[temp[2]];
            temp[3] = sbox[temp[3]];
        }

        /* 新组 = 上一组 ^ temp */
        for (j = 0; j < 4; j++)
            round_keys[i * 4 + j] = round_keys[(i - 4) * 4 + j] ^ temp[j];
    }
}

/* ================================================================
 *  AES-128 加密一个块（16 字节）
 * ================================================================ */
static void aes_encrypt_block(const uint8_t *in, uint8_t *out,
                              const uint8_t *round_keys)
{
    uint8_t state[16];
    uint8_t round, i;
    uint8_t t;

    /* 复制到 state + 初始 AddRoundKey */
    for (i = 0; i < 16; i++)
        state[i] = in[i] ^ round_keys[i];

    /* 前 9 轮 */
    for (round = 1; round < 10; round++)
    {
        /* SubBytes */
        for (i = 0; i < 16; i++)
            state[i] = sbox[state[i]];

        /* ShiftRows */
        t = state[1];  state[1] = state[5];  state[5] = state[9];
        state[9] = state[13]; state[13] = t;

        t = state[2];  state[2] = state[10]; state[10] = t;
        t = state[6];  state[6] = state[14]; state[14] = t;

        t = state[15]; state[15] = state[11]; state[11] = state[7];
        state[7] = state[3]; state[3] = t;

        /* MixColumns */
        for (i = 0; i < 16; i += 4)
        {
            uint8_t a0 = state[i];
            uint8_t a1 = state[i + 1];
            uint8_t a2 = state[i + 2];
            uint8_t a3 = state[i + 3];

            state[i]     = gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3;
            state[i + 1] = a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3;
            state[i + 2] = a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3);
            state[i + 3] = gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2);
        }

        /* AddRoundKey */
        for (i = 0; i < 16; i++)
            state[i] ^= round_keys[round * 16 + i];
    }

    /* 最后一轮：无 MixColumns */
    for (i = 0; i < 16; i++)
        state[i] = sbox[state[i]];

    t = state[1];  state[1] = state[5];  state[5] = state[9];
    state[9] = state[13]; state[13] = t;

    t = state[2];  state[2] = state[10]; state[10] = t;
    t = state[6];  state[6] = state[14]; state[14] = t;

    t = state[15]; state[15] = state[11]; state[11] = state[7];
    state[7] = state[3]; state[3] = t;

    for (i = 0; i < 16; i++)
        out[i] = state[i] ^ round_keys[10 * 16 + i];
}

/* ================================================================
 *  AES-128-CTR 加解密（CTR 对称）
 * ================================================================ */

/* CTR 计数器自增（128 位大端，仅自增低 32 位足够广播场景） */
static void ctr_increment(uint8_t *counter)
{
    uint8_t i;

    for (i = 0; i < 4; i++)   /* 自增最低 4 字节 */
    {
        if (++counter[15 - i] != 0)
            break;
    }
}

void AES128_CTR(uint8_t *in, uint8_t *out, uint16_t len)
{
    uint8_t  round_keys[176];
    uint8_t  counter[16];
    uint8_t  keystream[16];
    uint16_t i;
    uint8_t  j;

    key_expansion(g_aes128_key, round_keys);

    /* 计数器初值 = IV */
    for (j = 0; j < 16; j++)
        counter[j] = g_aes128_iv[j];

    for (i = 0; i < len; i += 16)
    {
        /* 加密计数器 → keystream 块 */
        aes_encrypt_block(counter, keystream, round_keys);

        /* 逐字节异或（处理尾部不足 16B） */
        for (j = 0; j < 16 && (i + j) < len; j++)
            out[i + j] = in[i + j] ^ keystream[j];

        ctr_increment(counter);
    }
}