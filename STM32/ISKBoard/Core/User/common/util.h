/**
  ******************************************************************************
  * @file    util.h
  * @brief   通用工具函数
  *          - isqrt: 整数平方根（避免 float / double）
  *          - dump_hex: 二进制数据 hex 打印（调试用）
  ******************************************************************************
  */
#ifndef __UTIL_H
#define __UTIL_H

#include <stdint.h>

/* 整数平方根（二分查找，最大 16 次迭代） */
uint32_t isqrt(uint32_t n);

/* 打印缓冲区 hex 内容 */
void dump_hex(const uint8_t *buf, uint16_t len);

#endif /* __UTIL_H */
