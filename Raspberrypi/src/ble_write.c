/**
  ******************************************************************************
  * @file    ble_write.c
  * @brief   BLE GATT 按需写入 — MQTT 下行指令 → 连接 ESP32 → 写特征值 → 断开
  *
  * 工作线程模型：
  *   MQTT 回调 ──ble_write_enqueue──▶ [mutex+cond 队列] ──▶ 工作线程
  *                                                               │
  *                                               ble_gatt_write: Connect
  *                                               → 等 ServicesResolved
  *                                               → 找 UUID 0xC302
  *                                               → WriteValue → Disconnect
  ******************************************************************************
  */
#include "ble_write.h"
#include "log.h"
#include "config.h"
#include <gio/gio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/* ---- 常量 ---- */
/* 16-bit UUID 0xC302 → 128-bit Bluetooth Base UUID */
#define CHAR_UUID   "0000c302-0000-1000-8000-00805f9b34fb"

#define CMD_QUEUE_SIZE   8       /* 指令队列深度 */

/* ---- 指令队列项 ---- */
typedef struct {
    uint8_t data[64];
    size_t  len;
} CmdItem_t;

/* ---- 模块全局 ---- */
static GDBusConnection *g_conn = NULL;

static CmdItem_t        g_queue[CMD_QUEUE_SIZE];
static int              g_head  = 0;   /* 写入位置 */
static int              g_tail  = 0;   /* 读取位置 */
static int              g_count = 0;
static pthread_mutex_t  g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cond  = PTHREAD_COND_INITIALIZER;
static pthread_t        g_thread;
static volatile int     g_running = 0;

/* ---- 前置声明 ---- */
static void ble_gatt_write(const uint8_t *frame, size_t len);
static void ble_device_path(char *mac, size_t mac_len, char *out);

/* ================================================================
 *  GATT 写入核心（工作线程内执行，同步阻塞）
 * ================================================================ */
/**
  * @brief  连接 ESP32 → 等 ServicesResolved → 写 0xC302 → 断开
  * @param  frame  完整协议帧（含 SOF/EOF）
  * @param  len    帧长
  * @note   同步阻塞 2~10 秒，全程有超时保护
  */
static void ble_gatt_write(const uint8_t *frame, size_t len)
{
    GError     *error        = NULL;
    GDBusProxy *device_proxy = NULL;
    GDBusProxy *om_proxy     = NULL;
    GVariant   *result       = NULL;
    gchar      *char_path    = NULL;
    char        device_path[64];
    int         i;

    LOG_INFO("[GATT] 开始连接 %s ...", g_cfg.target_mac);

    /* 由配置 MAC 构造 BlueZ 设备路径（大写、下划线） */
    ble_device_path(g_cfg.target_mac, strlen(g_cfg.target_mac), device_path);

    /* ---- 1. 创建设备代理 ---- */
    device_proxy = g_dbus_proxy_new_sync(
        g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.bluez", device_path, "org.bluez.Device1",
        NULL, &error);

    if (error)
     {
        LOG_ERROR("[GATT] 设备代理失败: %s", error->message);
        g_clear_error(&error);
        return;
    }

    /* ---- 2. 建立连接（已连接则忽略报错，继续走后续流程） ---- */
    result = g_dbus_proxy_call_sync(device_proxy, "Connect", NULL,
        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);

    if (error) 
    {
        LOG_WARN("[GATT] Connect 返回: %s (继续)", error->message);
        g_clear_error(&error);
    }
    if (result) 
    {
        g_variant_unref(result);
        result = NULL;
    }

    /* ---- 3. 轮询等待 GATT 服务发现完成 ---- */
    for (i = 0; i < 30; i++) 
    {
        GVariant *v = g_dbus_proxy_get_cached_property(
            device_proxy, "ServicesResolved");

        if (v) 
        {
            gboolean resolved = g_variant_get_boolean(v);
            g_variant_unref(v);
            if (resolved)
                break;
        }
        usleep(100000);   /* 100ms */
    }

    if (i >= 30) 
    {
        LOG_ERROR("[GATT] ServicesResolved 超时 (3s)");
        goto disconnect;
    }
    LOG_INFO("[GATT] 服务发现完成 (%d ms)", (i + 1) * 100);

    /* ---- 4. 遍历 GATT 对象树，找 UUID 匹配的特征值 ---- */
    om_proxy = g_dbus_proxy_new_sync(
        g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
        NULL, &error);

    if (error) 
    {
        LOG_ERROR("[GATT] ObjectManager 代理失败: %s", error->message);
        g_clear_error(&error);
        goto disconnect;
    }

    result = g_dbus_proxy_call_sync(om_proxy, "GetManagedObjects", NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if (error) 
    {
        LOG_ERROR("[GATT] GetManagedObjects 失败: %s", error->message);
        g_clear_error(&error);
        goto disconnect;
    }

    {
        GVariantIter *obj_iter;
        gchar        *obj_path;
        GVariantIter *iface_iter;

        g_variant_get(result, "(a{oa{sa{sv}}})", &obj_iter);

        while (g_variant_iter_loop(obj_iter, "{oa{sa{sv}}}", &obj_path, &iface_iter)) 
        {
            gchar    *iface_name;
            GVariant *props;

            while (g_variant_iter_loop(iface_iter, "{s@a{sv}}", &iface_name, &props)) 
            {
                if (g_strcmp0(iface_name, "org.bluez.GattCharacteristic1") == 0) 
                {

                    GVariant *uuid_var = g_variant_lookup_value(
                                          props, "UUID", G_VARIANT_TYPE_STRING);

                    if (uuid_var) 
                    {
                        const gchar *uuid = g_variant_get_string(uuid_var, NULL);

                        if (g_ascii_strcasecmp(uuid, CHAR_UUID) == 0) 
                        {
                            char_path = g_strdup(obj_path);
                            g_variant_unref(uuid_var);
                            break;
                        }
                        g_variant_unref(uuid_var);
                    }
                }
            }
            if (char_path)
                break;
        }
        g_variant_iter_free(obj_iter);
    }
    g_variant_unref(result);
    result = NULL;

    if (!char_path) {
        LOG_ERROR("[GATT] 未找到特征值 UUID=%s", CHAR_UUID);
        goto disconnect;
    }
    LOG_INFO("[GATT] 找到特征值: %s", char_path);

    /* ---- 5. WriteValue：把帧写入特征值 ---- */
    {
        GDBusProxy *char_proxy = g_dbus_proxy_new_sync(
            g_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
            "org.bluez", char_path,
            "org.bluez.GattCharacteristic1",
            NULL, &error);

        if (error) {
            LOG_ERROR("[GATT] 特征值代理失败: %s", error->message);
            g_clear_error(&error);
            g_free(char_path);
            goto disconnect;
        }

        /* 参数签名 WriteValue(ay value, a{sv} options) */
        GVariant *value  = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, frame, len, 1);
        GVariant *opts   = g_variant_new_array(
            G_VARIANT_TYPE("{sv}"), NULL, 0);
        GVariant *params = g_variant_new("(@ay@a{sv})", value, opts);

        result = g_dbus_proxy_call_sync(char_proxy, "WriteValue",
            params, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

        if (error) {
            LOG_ERROR("[GATT] WriteValue 失败: %s", error->message);
            g_clear_error(&error);
        } else {
            LOG_INFO("[GATT] 写入成功, %zu 字节", len);
            g_variant_unref(result);
        }

        g_free(char_path);
        g_object_unref(char_proxy);
    }

disconnect:
    /* ---- 6. 断开连接 ---- */
    g_dbus_proxy_call_sync(device_proxy, "Disconnect", NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    LOG_INFO("[GATT] 已断开");

    if (om_proxy)
        g_object_unref(om_proxy);
    if (device_proxy)
        g_object_unref(device_proxy);
}

static void *ble_write_thread(void *arg)
{
    CmdItem_t cmd;

    (void)arg;

    while (g_running)
    {
        pthread_mutex_lock(&g_mutex);

        while (g_count == 0 && g_running) /*初始化后就进入，等待cond_signal*/
        {
            pthread_cond_wait(&g_cond, &g_mutex); /*当前进程阻塞在这里*/
        }

        if (!g_running)
        {
            pthread_mutex_unlock(&g_mutex);
            break;
        }

        /* 出队 */
        cmd = g_queue[g_tail];
        g_tail = (g_tail + 1) % CMD_QUEUE_SIZE;
        g_count--;

        pthread_mutex_unlock(&g_mutex);

        /* 执行 GATT 写入（阻塞 2~10 秒） */
        ble_gatt_write(cmd.data, cmd.len);
    }

    return NULL;
}

static void ble_device_path(char *mac, size_t mac_len, char *out)
{
    uint8_t     i;
    char        path[64] = "/org/bluez/hci0/dev_";
    uint8_t     path_len = strlen(path);

    for(i = 0; i < mac_len; i++)
    {
        if(mac[i] == ':')
        {
            path[path_len] = '_';
        }
        else if(mac[i] >= 'a' && mac[i] <= 'f')
        {
            path[path_len] = mac[i] - 'a' + 'A';
        }
        else
        {
            path[path_len] = mac[i];
        }

        path_len += 1;
    }

    strncpy(out, path, 63);
    out[63] =  '\0';

    return ;
}

int ble_write_enqueue(const uint8_t *frame, size_t len)
{
  pthread_mutex_lock(&g_mutex);

  if (g_count >= CMD_QUEUE_SIZE)
  {
    pthread_mutex_unlock(&g_mutex);  
    return -1;
  }

  memcpy(g_queue[g_head].data, frame, len);
  g_queue[g_head].len = len;
  g_head = (g_head + 1) % CMD_QUEUE_SIZE;
  g_count++;
  
  pthread_cond_signal(&g_cond);

  pthread_mutex_unlock(&g_mutex);
  return 0;
}

int ble_write_init(void *conn)
{
    g_conn = (GDBusConnection *)conn;
    g_running = 1;
    pthread_create(&g_thread, NULL, ble_write_thread, NULL);

    return 0;
}

void ble_write_cleanup(void)
{
    g_running = 0;

    pthread_cond_signal(&g_cond);
    pthread_join(g_thread, NULL);
}