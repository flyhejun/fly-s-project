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
#include <string.h>
#include <strings.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include "log.h"
#include "comm_parse.h"
#include "ble_write.h"
#include "config.h"

#define DEVICE_ID       "pi01"

#define TOPIC_CMD       "v1/devices/me/rpc/request/+"  /* ThingsBoard RPC 请求 topic（下行） */
#define TOPIC_STATUS    "v1/devices/me/telemetry"       /* 上行状态走遥测 */

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
        mosquitto_publish(mosq, NULL, TOPIC_STATUS,
                          strlen("{\"type\":\"status\",\"state\":\"online\"}"),
                          "{\"type\":\"status\",\"state\":\"online\"}", 0, false);
    }
    else
    {
        LOG_WARN("MQTT 连接失败，rc=%d (%s)", rc, mosquitto_connack_string(rc));
    }
}

static void on_message(struct mosquitto *mosq, void *userdata,
                       const struct mosquitto_message *msg)
{
    cJSON       *root;
    cJSON       *method;
    cJSON       *params;
    cJSON       *param;
    cJSON       *value;
    const char  *request_id;
    uint8_t      type;
    int          plen;

    uint8_t payload[16];
    uint8_t buf[32];
    char    resp_topic[96];

    LOG_INFO("MQTT 收到 topic=%s", msg->topic);

    /* 1. 从 topic 提取请求号：最后一个 '/' 之后就是 RPC 请求 id */
    request_id = strrchr(msg->topic, '/');
    if (request_id == NULL)
    {
        LOG_WARN("RPC topic 格式异常: %s", msg->topic);
        return;
    }
    request_id++;

    /* 2. 解析 JSON：{"method":"...","params":{...}} */
    root = cJSON_ParseWithLength((const char *)msg->payload, msg->payloadlen);
    if (root == NULL)
    {
        LOG_WARN("RPC JSON 解析失败");
        return;
    }

    method = cJSON_GetObjectItem(root, "method");
    params = cJSON_GetObjectItem(root, "params");
    if (cJSON_IsString(method))
    {
        LOG_INFO("收到 RPC: method=%s", method->valuestring);

        /* 3. 方法名 → 帧 TYPE 分发 */
        if (strcasecmp(method->valuestring, "setThreshold") == 0)
        {
            type = 0x81;

            /* paramId（1~4）→ payload[0] */
            param = cJSON_GetObjectItem(params, "paramId");
            if (!cJSON_IsNumber(param))
            {
                LOG_WARN("setThreshold 缺少 paramId");
                cJSON_Delete(root);
                return;
            }
            payload[0] = (uint8_t)param->valueint;

            /* value → payload[1..4] 小端 4 字节 */
            value = cJSON_GetObjectItem(params, "value");
            if (!cJSON_IsNumber(value))
            {
                LOG_WARN("setThreshold 缺少 value");
                cJSON_Delete(root);
                return;
            }
            payload[1] = (uint8_t)(value->valueint & 0xFF);
            payload[2] = (uint8_t)((value->valueint >> 8) & 0xFF);
            payload[3] = (uint8_t)((value->valueint >> 16) & 0xFF);
            payload[4] = (uint8_t)((value->valueint >> 24) & 0xFF);

            plen = comm_pack_cmd(buf, sizeof(buf), type, payload, 5);
            if (plen <= 0)
            {
                LOG_ERROR("设置阈值帧打包失败");
                cJSON_Delete(root);
                return;
            }
            if (ble_write_enqueue(buf, plen) != 0)
            {
                LOG_WARN("BLE 写入队列已满，丢弃指令");
            }
        }
        else if (strcasecmp(method->valuestring, "alarmCancel") == 0)
        {
            type = 0x83;

            plen = comm_pack_cmd(buf, sizeof(buf), type, payload, 0);
            if (plen <= 0)
            {
                LOG_ERROR("alarmCancel 帧打包失败");
                cJSON_Delete(root);
                return;
            }
            if (ble_write_enqueue(buf, plen) != 0)
            {
                LOG_WARN("BLE 写入队列已满，丢弃指令");
            }
        }
        else if (strcasecmp(method->valuestring, "testLed") == 0)
        {
            type = 0x84;

            value = cJSON_GetObjectItem(params, "value");
            if (!cJSON_IsNumber(value))
            {
                LOG_WARN("testLed 缺少 value");
                cJSON_Delete(root);
                return;
            }

            payload[0] = (uint8_t)value->valueint;
            plen = comm_pack_cmd(buf, sizeof(buf), type, payload, 1);
            if (plen <= 0)
            {
                LOG_ERROR("testLed 帧打包失败");
                cJSON_Delete(root);
                return;
            }
            if (ble_write_enqueue(buf, plen) != 0)
            {
                LOG_WARN("BLE 写入队列已满，丢弃指令");
            }
        }
        else if (strcasecmp(method->valuestring, "testBuzzer") == 0)
        {
            type = 0x85;

            value = cJSON_GetObjectItem(params, "value");
            if (!cJSON_IsNumber(value))
            {
                LOG_WARN("testBuzzer 缺少 value");
                cJSON_Delete(root);
                return;
            }

            payload[0] = (uint8_t)value->valueint;
            plen = comm_pack_cmd(buf, sizeof(buf), type, payload, 1);
            if (plen <= 0)
            {
                LOG_ERROR("testBuzzer 帧打包失败");
                cJSON_Delete(root);
                return;
            }
            if (ble_write_enqueue(buf, plen) != 0)
            {
                LOG_WARN("BLE 写入队列已满，丢弃指令");
            }
        }
        else if (strcasecmp(method->valuestring, "queryStatus") == 0)
        {
            type = 0x87;

            plen = comm_pack_cmd(buf, sizeof(buf), type, payload, 0);
            if (plen <= 0)
            {
                LOG_ERROR("queryStatus 帧打包失败");
                cJSON_Delete(root);
                return;
            }
            if (ble_write_enqueue(buf, plen) != 0)
            {
                LOG_WARN("BLE 写入队列已满，丢弃指令");
            }
        }
        else
        {
            LOG_WARN("未知 RPC 方法: %s", method->valuestring);
            cJSON_Delete(root);
            return;
        }

        /* 4. 回包 success：ThingsBoard 收到才认为 RPC 完成，否则一直 pending */
        snprintf(resp_topic, sizeof(resp_topic), "v1/devices/me/rpc/response/%s", request_id);
        mosquitto_publish(mosq, NULL, resp_topic,
                          strlen("{\"success\":true}"), "{\"success\":true}", 0, false);
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

    mosquitto_username_pw_set(mosq, g_cfg.username, g_cfg.password);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);

    /* 带重试的连接：mosquitto_loop_start 会持续尝试（底层自带重连） */
    int rc = mosquitto_connect(mosq, g_cfg.host, g_cfg.port, 60);
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

    /* ---- PID 文件锁：防重复启动 ---- */
    {
        #define PID_FILE "/tmp/isk_gateway.pid"
        int  fd;
        char buf[16];
        int  pid_old = 0;

        fd = open(PID_FILE, O_RDWR | O_CREAT, 0644);
        if (fd < 0)
        {
            LOG_ERROR("无法创建 PID 文件 %s", PID_FILE);
            return 1;
        }

        /* 尝试加写锁（非阻塞），已锁 = 有另一个实例在跑 */
        struct flock fl = { F_WRLCK, SEEK_SET, 0, 0, 0 };
        if (fcntl(fd, F_SETLK, &fl) == -1)
        {
            /* 读旧 PID 打日志 */
            if (read(fd, buf, sizeof(buf) - 1) > 0)
            {
                buf[sizeof(buf) - 1] = '\0';
                pid_old = atoi(buf);
            }
            LOG_ERROR("已有实例在运行 (PID=%d)，拒绝启动", pid_old);
            close(fd);
            return 1;
        }

        /* 截断并写入当前 PID */
        ftruncate(fd, 0);
        lseek(fd, 0, SEEK_SET);
        snprintf(buf, sizeof(buf), "%d\n", getpid());
        write(fd, buf, strlen(buf));
        /* fd 保持打开，进程退出时内核自动释放锁 */
    }

    /* 加载配置（文件不存在则用默认值） */
    Config_Load("./isk_gateway.conf");

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

    /* 4. BLE GATT 写入子系统（MQTT 下行指令 → 连接 → 写特征值 → 断开） */
    ble_write_init(conn);

    /* 5. 主循环 */
    loop = g_main_loop_new(NULL, FALSE);
    LOG_INFO("进入主循环");
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    ble_write_cleanup();
    mosquitto_loop_stop(g_mosq, true);
    mosquitto_destroy(g_mosq);
    mosquitto_lib_cleanup();
    g_object_unref(conn);
    return 0;
}
