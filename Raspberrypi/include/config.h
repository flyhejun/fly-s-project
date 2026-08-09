/**
  ******************************************************************************
  * @file    config.h
  * @brief   网关配置 — 从 isk_gateway.conf 读取，缺文件回退默认值
  *
  * 设计：
  *   - key=value 纯文本格式，一行一个，空行 / # 注释跳过
  *   - 启动时 Config_Load() 一次，填充全局 g_cfg
  *   - 文件缺失或 key 未出现 → 用编译期默认值（与旧硬编码一致，行为不回归）
  ******************************************************************************
  */
#ifndef ISK_CONFIG_H
#define ISK_CONFIG_H

/* ---- 配置结构体：集中所有原本硬编码的运行时参数 ---- */
typedef struct
{
    char host[64];        /* MQTT 服务器地址 */
    int  port;            /* MQTT 端口       */
    char username[32];    /* MQTT 账号       */
    char password[32];    /* MQTT 密码       */
    char target_mac[20];  /* ESP32 BLE MAC   */
} GatewayConfig_t;

/* 全局配置实例（config.c 中定义并初始化默认值） */
extern GatewayConfig_t g_cfg;

/**
  * @brief  从配置文件加载覆盖 g_cfg
  * @param  path 配置文件路径（如 "./isk_gateway.conf"）
  * @retval 0  成功（文件存在并解析）
  *         -1 文件不存在（g_cfg 保持默认值）
  */
int Config_Load(const char *path);

#endif /* ISK_CONFIG_H */