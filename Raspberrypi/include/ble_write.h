/**
  ******************************************************************************
  * @file    ble_write.h
  * @brief   BLE GATT 按需写入 — MQTT 下行指令 → 连接 ESP32 → 写特征值 → 断开
  *
  * 使用方式：
  *   1. main() 中调用 ble_write_init(conn) 启动工作线程
  *   2. MQTT on_message 中调用 ble_write_enqueue(frame, len) 入队指令帧
  *   3. 程序退出前调用 ble_write_cleanup()
  ******************************************************************************
  */
#ifndef BLE_WRITE_H
#define BLE_WRITE_H

#include <stdint.h>
#include <stddef.h>

/**
  * @brief  初始化 BLE 写入子系统（启动工作线程）
  * @param  conn  D-Bus system bus 连接（main.c 中创建）
  * @retval 0  成功
  *         -1 线程创建失败
  */
int ble_write_init(void *conn);

/**
  * @brief  将指令帧入队，等待工作线程连接 ESP32 并写入 GATT
  * @param  frame  完整协议帧（含 SOF/EOF，由 comm_pack_cmd 打包）
  * @param  len    帧长度（字节）
  * @retval 0  入队成功
  *         -1 队列满（指令丢失，上层可重试）
  * @note   非阻塞，可在 MQTT 回调中直接调用
  */
int ble_write_enqueue(const uint8_t *frame, size_t len);

/**
  * @brief  停止工作线程并清理资源
  */
void ble_write_cleanup(void);

#endif /* BLE_WRITE_H */