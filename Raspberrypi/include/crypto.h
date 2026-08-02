/**
  ******************************************************************************
  * @file    crypto.h
  * @brief   树莓派侧 AES-128-CTR 加解密（openssl EVP 实现）
  *
  * 与 STM32 侧 aes128.c 对应：解密广播 payload。
  * key/IV 必须与 STM32 的 g_aes128_key / g_aes128_iv 逐字节一致。
  ******************************************************************************
  */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* ---- key / IV 常量（与 STM32 aes128.h 一致） ---- */
#define AES_KEY_LEN  16

extern const uint8_t g_aes_key[AES_KEY_LEN];
extern const uint8_t g_aes_iv[16];

/**
  * @brief  AES-128-CTR 加解密（CTR 对称，加解密共用）
  * @param  in   输入缓冲区
  * @param  out  输出缓冲区（可与 in 相同）
  * @param  len  字节数（任意长度，无需块对齐）
  * @retval 0  成功
  *         -1 失败（openssl 内部错误）
  */
int aes128_ctr_crypt(const uint8_t *in, uint8_t *out, size_t len);

#endif /* CRYPTO_H */