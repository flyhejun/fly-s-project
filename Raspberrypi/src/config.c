/**
  ******************************************************************************
  * @file    config.c
  * @brief   网关配置加载 — 从 key=value 文本文件覆盖默认值
  *
  * 用法：main() 启动时调用 Config_Load("./isk_gateway.conf")。
  *   文件存在 → 逐行解析，覆盖 g_cfg 对应字段
  *   文件不存在 → 保持 g_cfg 默认值，LOG_WARN 提醒
  ******************************************************************************
  */
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "log.h"

/* 全局配置实例：默认值，文件缺失时回退。
 * port 默认值随架构迁移改为 ThingsBoard(1884)；
 * 真实令牌只放 gitignored 的 isk_gateway.conf，不进代码库（避免泄密） */
GatewayConfig_t g_cfg = {
    .host       = "121.40.252.238",
    .port       = 1884,
    .username   = "flyzzz",
    .password   = "chanhjf17",
    .target_mac = "58:8c:81:0e:4e:16",
};

int Config_Load(const char *path)
{
    char        line[128];
    size_t     pos;

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
    {
        LOG_WARN("配置文件 %s 不存在，使用默认配置", path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp))
    {
       pos = strcspn(line, "\n");
       line[pos] = '\0';

       if(line[0] == '\0' || line[0] == '#')
       {
            continue;
       }
       else if (strncmp(line, "host=", 5) == 0)
       {
            strcpy(g_cfg.host, line + 5);
       }
       else if(strncmp(line, "port=", 5) == 0)
       {
            sscanf(line + 5, "%d", &g_cfg.port);
       }
       else if(strncmp(line, "username=", 9) == 0)
       {
            strcpy(g_cfg.username, line + 9);
       }
       else if(strncmp(line, "password=", 9) == 0)
       {
            strcpy(g_cfg.password, line + 9);
       }
       else if(strncmp(line, "target_mac=", 11) == 0)
       {
            strcpy(g_cfg.target_mac, line + 11);
       }
       else
       {
            continue;
       }
    }

    fclose(fp);
    LOG_INFO("配置文件 %s 已加载", path);
    return 0;
}