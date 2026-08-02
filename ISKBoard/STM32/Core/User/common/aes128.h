/**
  ******************************************************************************
  * @file    aes128.h
  * @brief   AES-128 软件实现 + CTR 加解密
  *
  * 用途：BLE 广播 payload 加密（STM32 侧）。树莓派侧用 openssl 对应解密。
  *
  * 设计：
  *   - 固定 key + 固定 IV（两端硬编码一致，方案 A，不加密钥分发）
  *   - CTR 流模式：密文与明文等长，帧结构/长度不变，符合 31B 广播限制
  *   - CTR 加解密是同一个函数（异或对称）
  ******************************************************************************
  */
#ifndef __AES128_H
#define __AES128_H

#include <stdint.h>

/* ---- 固定密钥与 IV（两端必须一致） ---- */
#define AES128_KEY_LEN  16
#define AES128_BLOCK    16

extern const uint8_t g_aes128_key[AES128_KEY_LEN];
extern const uint8_t g_aes128_iv[AES128_BLOCK];

/**
  * @brief  AES-128-CTR 加解密（CTR 对称，加解密共用）
  * @param  in   输入缓冲区
  * @param  out  输出缓冲区（可与 in 相同）
  * @param  len  字节数（任意长度，无需块对齐）
  * @note   使用固定 key/IV，无状态，可随时调用
  */
void AES128_CTR(uint8_t *in, uint8_t *out, uint16_t len);

#endif /* __AES128_H */
