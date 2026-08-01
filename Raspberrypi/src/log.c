/**
  ******************************************************************************
  * @file    log.c
  * @brief   简易日志实现 — 同时写文件和 stderr
  ******************************************************************************
  */
#include "log.h"
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

static FILE    *g_log_file  = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *LEVEL_STR[] = { "INFO", "WARN", "ERROR" };

void log_init(const char *path)
{
    if (path == NULL)
        return;

    g_log_file = fopen(path, "a");
    if (g_log_file == NULL)
    {
        fprintf(stderr, "[LOG] 无法打开日志文件 %s，仅输出到 stderr\n", path);
    }
    else
    {
        setbuf(g_log_file, NULL);   /* 行缓冲：每条日志立即落盘 */
    }
}

void log_write(LogLevel_t level, const char *fmt, ...)
{
    va_list  args;
    time_t   now;
    struct tm tm_info;
    char     time_buf[24];

    /* 时间戳 */
    now = time(NULL);
    localtime_r(&now, &tm_info);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

    pthread_mutex_lock(&g_log_mutex);

    /* stderr（终端实时看） */
    fprintf(stderr, "[%s] [%s] ", time_buf, LEVEL_STR[level]);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    /* 文件（持久保存） */
    if (g_log_file != NULL)
    {
        fprintf(g_log_file, "[%s] [%s] ", time_buf, LEVEL_STR[level]);
        va_start(args, fmt);
        vfprintf(g_log_file, fmt, args);
        va_end(args);
        fprintf(g_log_file, "\n");
    }

    pthread_mutex_unlock(&g_log_mutex);
}