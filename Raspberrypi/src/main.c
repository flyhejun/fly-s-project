/**
  ******************************************************************************
  * @file    main.c
  * @brief   ISKBoard 树莓派网关 — BLE 接收 + MQTT 上传
  *
  * 架构：
  *   GLib GMainLoop（BLE D-Bus 事件） + mosquitto_loop_start（MQTT 后台线程）
  *   两个事件循环并存，互不阻塞。
  *
  * 容错：MQTT 断连自动重试，BLE 扫描失败不退出。
  ******************************************************************************
  */
#include <gio/gio.h>
#include <mosquitto.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "log.h"

#define MQTT_HOST       "121.40.252.238"
#define MQTT_PORT       1883
#define MQTT_USERNAME   "flyzzz"
#define MQTT_PASSWORD   "chanhjf17"
#define DEVICE_ID       "pi01"

#define TOPIC_CMD       "fall_detection/" DEVICE_ID "/cmd"
#define TOPIC_STATUS    "fall_detection/" DEVICE_ID "/status"

/* 全局 MQTT 实例（ble_central.c 回调中使用） */
struct mosquitto *g_mosq = NULL;

/* BLE 启动函数（定义在 ble_central.c） */
void ble_start(GDBusConnection *conn);

/* ================================================================
 *  MQTT 回调
 * ================================================================ */

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
    if (rc == 0)
    {
        LOG_INFO("MQTT 连接成功");
        mosquitto_subscribe(mosq, NULL, TOPIC_CMD, 1);
        mosquitto_publish(mosq, NULL, TOPIC_STATUS, strlen("online"),
                          "online", 0, false);
    }
    else
    {
        LOG_WARN("MQTT 连接失败，rc=%d (%s)", rc, mosquitto_connack_string(rc));
    }
}

static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    cJSON *root;
    cJSON *cmd;

    LOG_INFO("MQTT 收到 topic=%s", msg->topic);

    root = cJSON_ParseWithLength((const char *)msg->payload, msg->payloadlen);
    if (root == NULL)
    {
        LOG_WARN("MQTT 指令 JSON 解析失败");
        return;
    }

    cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd))
    {
        LOG_INFO("收到指令: %s", cmd->valuestring);
        /* TODO: 解析 cmd → 协议帧 → BLE Write 发给 ESP32 */
    }

    cJSON_Delete(root);
}

/* ================================================================
 *  MQTT 初始化（带重试）
 * ================================================================ */

static struct mosquitto *mqtt_init(void)
{
    struct mosquitto *mosq;

    mosquitto_lib_init();

    mosq = mosquitto_new(DEVICE_ID, true, NULL);
    if (!mosq)
    {
        LOG_ERROR("创建 mosquitto 实例失败");
        return NULL;
    }

    mosquitto_username_pw_set(mosq, MQTT_USERNAME, MQTT_PASSWORD);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

    /* 带重试的连接：mosquitto_loop_start 会持续尝试（底层自带重连） */
    int rc = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        LOG_WARN("MQTT 首次连接失败: %s，后台线程将继续重试", mosquitto_strerror(rc));
    }

    mosquitto_loop_start(mosq);
    return mosq;
}

/* ================================================================
 *  main
 * ================================================================ */

int main(void)
{
    GError          *error = NULL;
    GDBusConnection *conn;
    GMainLoop       *loop;

    /* 初始化日志 */
    log_init("./isk_gateway.log");
    LOG_INFO("ISKBoard 网关启动");

    /* 1. MQTT：创建失败才退出（内存不足），连接失败后台重试 */
    g_mosq = mqtt_init();
    if (g_mosq == NULL)
    {
        LOG_ERROR("MQTT 初始化失败（内存不足），退出");
        return 1;
    }

    /* 2. D-Bus 连接（BlueZ 必须） */
    conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (error)
    {
        LOG_ERROR("D-Bus 连接失败: %s（BlueZ 未运行？）", error->message);
        g_error_free(error);
        /* 不退出：可能 BlueZ 延迟启动，继续尝试 */
        sleep(2);
        conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (error)
        {
            LOG_ERROR("D-Bus 重试失败，退出");
            g_error_free(error);
            return 2;
        }
    }
    LOG_INFO("System Bus 连接成功");

    /* 3. BLE 广播接收（ESP32 广播包 → ManufacturerData → 帧） */
    ble_start(conn);

    /* 4. 主循环 */
    loop = g_main_loop_new(NULL, FALSE);
    LOG_INFO("进入主循环");
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    mosquitto_loop_stop(g_mosq, true);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    g_object_unref(conn);
    return 0;
}
