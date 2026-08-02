/**
  ******************************************************************************
  * @file    crypto.c
  * @brief   树莓派侧 AES-128-CTR 加解密实现（openssl EVP）
  *
  * 对应 STM32 侧 aes128.c：固定 key/IV，CTR 流模式，密文与明文等长。
  ******************************************************************************
  */
#include "crypto.h"
#include <openssl/evp.h>
#include <string.h>

/* ---- key / IV 常量（与 STM32 aes128.c 一致） ---- */
const uint8_t g_aes_key[AES_KEY_LEN] = {
    0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
};

const uint8_t g_aes_iv[16] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00
};

int aes128_ctr_crypt(const uint8_t *in, uint8_t *out, size_t len)
{
    EVP_CIPHER_CTX *ctx;
    int   out_len = 0;
    int   final_len = 0;
    int   rc;

    if (in == NULL || out == NULL)
        return -1;

    ctx = EVP_CIPHER_CTX_new();
    if (ctx == NULL)
        return -1;

    /* 初始化 AES-128-CTR 加密上下文（CTR 下加密=解密，共用此函数） */
    rc = EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, g_aes_key, g_aes_iv);
    if (rc != 1)
        goto fail;

    /* 就地加密：in/out 可以是同一缓冲区 */
    if (EVP_EncryptUpdate(ctx, out, &out_len, in, (int)len) != 1)
        goto fail;

    /* CTR 无填充，Final 应为 0 输出 */
    if (EVP_EncryptFinal_ex(ctx, out + out_len, &final_len) != 1)
        goto fail;

    EVP_CIPHER_CTX_free(ctx);
    return 0;

fail:
    EVP_CIPHER_CTX_free(ctx);
    return -1;
}