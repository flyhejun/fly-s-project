/**
  ******************************************************************************
  * @file    log.h
  * @brief   简易日志模块 — 写文件，带时间戳
  ******************************************************************************
  */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/* 日志级别 */
typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} LogLevel_t;

/**
  * @brief  初始化日志文件
  * @param  path  日志文件路径（如 "/var/log/isk_gateway.log"）
  *              传 NULL 则仅输出到 stderr
  */
void log_init(const char *path);

/**
  * @brief  写一条日志
  * @param  level  INFO / WARN / ERROR
  * @param  fmt    printf 格式字符串
  * @param  ...    可变参数
  */
void log_write(LogLevel_t level, const char *fmt, ...);

/* 快捷宏 */
#define LOG_INFO(fmt, ...)   log_write(LOG_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   log_write(LOG_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  log_write(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
