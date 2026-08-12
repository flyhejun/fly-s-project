/**
  ******************************************************************************
  * @file    ble_central.c
  * @brief   BLE 广播接收 — 从 ESP32 广播包 ManufacturerData 中提取帧
  *
  * 工作方式（纯扫描，不连接）：
  *   1. StartDiscovery（Transport=le 纯 LE 扫描）
  *   2. 发现 ESP32 → watch_device + 500ms 定时轮询 Device1.ManufacturerData
  *   3. 轮询读到数据 → 解析帧 → MQTT
  *
  * 注意：ESP32 需要把帧数据写入 BLE 广播包 Manufacturer Specific Data 字段。
  ******************************************************************************
  */
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "comm_parse.h"
#include "mqtt_publish.h"
#include "log.h"
#include "config.h"

/* MQTT 实例（main.c 中初始化） */
extern struct mosquitto *g_mosq;

/* 找到 ESP32 后用于定时轮询 ManufacturerData */
static GDBusConnection *g_dev_conn = NULL;
static char             g_dev_path[128];
static guint            s_poll_src = 0;   /* 500ms 轮询定时器句柄（防自愈时重复添加） */

/* ---- 数据新鲜度看门狗状态 ---- */
#define FRESH_WATCHDOG_MS   15000   /* 15s 无序号更新 = 判定失联 */
static uint8_t   s_last_frame[32];   /* 去重：上一帧内容（不含序号） */
static uint16_t  s_last_len   = 0;
static uint8_t   s_last_seq   = 0xFF;  /* 上一广播序号（初值确保首帧触发） */
static gint64    g_last_seq_ms = 0;    /* 最近序号更新时刻（monotonic ms） */

/* ---- 深度复位冷却：HCI/bluetoothd 重启用，防频繁锤控制器崩溃 ---- */
#define DEEP_RESET_COOLDOWN_MS  300000   /* 5 分钟一次 */
static gint64    s_last_deep_reset_ms = 0;

/* ble_write.c 定义的标志：下行 GATT 期间暂停扫描，poll 需配合跳过自愈 */
extern volatile int g_scan_suspended;

/* ---- 前置声明 ---- */
static void process_mfg_data(GVariant *mfg_data);
static void process_advertising_data(GVariant *props);
static gboolean check_existing_devices(GDBusConnection *conn);
static void restart_discovery(GDBusConnection *conn);

/* ================================================================
 *  定时轮询 ManufacturerData
 * ================================================================ */

/**
  * @brief  GLib 定时回调：读缓存属性 → 解析帧
  *
  * 由于 BlueZ 默认 DuplicateData=true，后续广播更新不触发
  * PropertiesChanged。这里用定时轮询绕过该限制，直接从
  * Device1 缓存属性读取 ManufacturerData。
  */
static gboolean poll_manufacturer_data(gpointer user_data)
{
    GDBusProxy *dev_proxy;
    GVariant   *props;
    static int  tick = 0;
    static int  fail_cnt = 0;   /* 连续失败计数（自愈触发阈值） */
    (void)user_data;

    /* 下行 GATT 期间扫描被暂停：跳过本轮，避免误触发自愈 */
    if (g_scan_suspended)
        return G_SOURCE_CONTINUE;

    if (g_dev_path[0] == '\0')
        return G_SOURCE_CONTINUE;

    dev_proxy = g_dbus_proxy_new_sync(
        g_dev_conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.bluez", g_dev_path, "org.bluez.Device1",
        NULL, NULL);

    if (dev_proxy)
    {
        props = g_dbus_proxy_get_cached_property(dev_proxy, "ManufacturerData");
        if (props)
        {
            gint64 now_ms = g_get_monotonic_time() / 1000;

            /* 首次轮询：以当前时间作基准 */
            if (g_last_seq_ms == 0)
                g_last_seq_ms = now_ms;

            if (now_ms - g_last_seq_ms > FRESH_WATCHDOG_MS)
            {
                /* 缓存有数据但序号久未更新 → 控制器可能已卡死，
                 * 之前的 fail_cnt 逻辑只认"缓存为空"，这里补上"数据陈旧" */
                if (++fail_cnt >= 8)
                {
                    fail_cnt = 0;
                    g_dev_path[0] = '\0';
                    s_last_len = 0;   /* 清去重，防恢复后首帧被吞 */
                    LOG_WARN("[POLL] 数据 %dms 无更新，重启扫描", FRESH_WATCHDOG_MS);
                    /* 数据陈旧说明控制器不投递新广播了，缓存里的设备对象也是
                     * 陈旧的 —— check_existing_devices 只重看缓存救不回来。
                     * 必须真正重启扫描（内部含 Stop/Start → HCI → bluetoothd 升级） */
                    restart_discovery(g_dev_conn);
                }
            }
            else
            {
                fail_cnt = 0;   /* 数据新鲜 → 设备正常，重置失败计数 */
                process_mfg_data(props);
            }
            g_variant_unref(props);
        }
        else
        {
            /* 连续 8 次（4 秒）读不到 → BlueZ 设备对象可能已失效，重新发现 */
            if (++fail_cnt >= 8)
            {
                fail_cnt = 0;
                g_dev_path[0] = '\0';
                s_last_len = 0;
                LOG_WARN("[POLL] 连续读不到 ManufacturerData，重新发现设备");
                if (!check_existing_devices(g_dev_conn))
                {
                    restart_discovery(g_dev_conn);
                }
            }
            else if (++tick % 20 == 1)
            {
                LOG_INFO("[POLL] ManufacturerData 缓存为空 (tick=%d)", tick);
            }
        }
        g_object_unref(dev_proxy);
    }
    else
    {
        /* 设备代理创建失败（对象被移除）同样自愈 */
        if (++fail_cnt >= 8)
        {
            fail_cnt = 0;
            g_dev_path[0] = '\0';
            s_last_len = 0;
            LOG_WARN("[POLL] 设备代理失效，重新发现设备");
            if (!check_existing_devices(g_dev_conn))
            {
                restart_discovery(g_dev_conn);
            }
        }
        else if (++tick % 20 == 1)
        {
            LOG_WARN("[POLL] 设备代理创建失败: %s", g_dev_path);
        }
    }

    return G_SOURCE_CONTINUE;
}

/* ================================================================
 *  内部辅助
 * ================================================================ */

/**
  * @brief  解析 ManufacturerData 字典（a{qv}）中的每一组厂商数据
  *
  * BlueZ 把广播包中的 Manufacturer Specific Data（AD type 0xFF）
  * 映射为 Device1 接口的 ManufacturerData 属性，类型 a{qv}：
  *   - key:  uint16 厂商 ID
  *   - value: variant → byte array（即原始帧数据）
  */
static void process_mfg_data(GVariant *mfg_data)
{
    GVariantIter iter;
    guint16      mfg_id;
    GVariant    *value;

    g_variant_iter_init(&iter, mfg_data);

    while (g_variant_iter_next(&iter, "{qv}", &mfg_id, &value))
    {
        gsize        len;
        const guint8 *data;
        ParsedFrame_t frame;

        data = g_variant_get_fixed_array(value, &len, sizeof(guint8));

        /* 广播数据 = 协议帧 + 末尾 1 字节滚动序号（STM32 附加）。
         * 序号变化 → 广播还在更新（存活信号，看门狗依据）；
         * 去重只按帧内容，静止时帧相同仍去重，不刷 MQTT */
        if (len > 1)
        {
            uint8_t seq       = data[len - 1];
            gsize   frame_len = len - 1;

            if (seq != s_last_seq)
            {
                s_last_seq    = seq;
                g_last_seq_ms = g_get_monotonic_time() / 1000;
            }

            if (comm_parse_frame(data, frame_len, &frame))
            {
                if (frame_len == s_last_len
                    && memcmp(data, s_last_frame, frame_len) == 0)
                {
                    g_variant_unref(value);
                    continue;
                }
                memcpy(s_last_frame, data, frame_len);
                s_last_len = (uint16_t)frame_len;
                LOG_INFO("[ADV] mfg_id=0x%04X, len=%zu", mfg_id, len);

                LOG_INFO("[ADV] 解析帧成功 type=%02X", frame.type);

                if (g_mosq)
                    mqtt_publish_frame(g_mosq, &frame);
            }
            else
            {
                LOG_INFO("[ADV] 未识别的数据：%zu 字节", len);
            }
        }

        g_variant_unref(value);
    }
}

/**
  * @brief  从设备属性字典（a{sv}）中查找 ManufacturerData 并解析
  * @note   供 PropertiesChanged / InterfacesAdded 传入整个属性字典时使用
  */
static void process_advertising_data(GVariant *props)
{
    GVariant *mfg_data;

    mfg_data = g_variant_lookup_value(props, "ManufacturerData",
                                      G_VARIANT_TYPE("a{qv}"));
    if (mfg_data == NULL)
        return;

    process_mfg_data(mfg_data);
    g_variant_unref(mfg_data);
}

/* ================================================================
 *  D-Bus 信号回调
 * ================================================================ */

/**
  * @brief  设备属性变化回调
  *
  * ESP32 每次更新广播数据（停止→重发），BlueZ 会更新 Device1 的
  * ManufacturerData / RSSI 等属性，触发 PropertiesChanged。
  */
static void on_device_properties_changed(GDBusConnection *connection,
                                          const gchar      *sender_name,
                                          const gchar      *object_path,
                                          const gchar      *interface_name,
                                          const gchar      *signal_name,
                                          GVariant         *parameters,
                                          gpointer          user_data)
{
    const gchar *iface;
    GVariant    *changed_props;
    GVariant    *invalidated_props;

    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)signal_name;
    (void)user_data;

    g_variant_get(parameters, "(&s@a{sv}@as)",
                  &iface, &changed_props, &invalidated_props);

    if (g_strcmp0(iface, "org.bluez.Device1") == 0)
    {
        process_advertising_data(changed_props);
    }

    g_variant_unref(changed_props);
    g_variant_unref(invalidated_props);
}

/**
  * @brief  订阅目标设备的广播数据更新
  */
static void watch_device(GDBusConnection *conn, const gchar *device_path)
{
    g_dbus_connection_signal_subscribe(
        conn,
        "org.bluez",
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        device_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_device_properties_changed,
        NULL,
        NULL);

    LOG_INFO("已监听广播数据: %s", device_path);
}

/* ================================================================
 *  设备发现
 * ================================================================ */

/**
  * @brief  扫描结果回调：新设备出现
  */
static void on_interfaces_added(GDBusConnection *connection,
                                 const gchar      *sender_name,
                                 const gchar      *object_path,
                                 const gchar      *interface_name,
                                 const gchar      *signal_name,
                                 GVariant         *parameters,
                                 gpointer          user_data)
{
    const gchar *obj_path;
    GVariant    *interfaces;

    (void)sender_name;
    (void)interface_name;
    (void)signal_name;
    (void)user_data;

    g_variant_get(parameters, "(&o@a{sa{sv}})", &obj_path, &interfaces);

    GVariantIter iter;
    gchar       *iface_name;
    GVariant    *props;

    g_variant_iter_init(&iter, interfaces);

    while (g_variant_iter_loop(&iter, "{s@a{sv}}", &iface_name, &props))
    {
        if (g_strcmp0(iface_name, "org.bluez.Device1") == 0)
        {
            GVariant *addr_variant = g_variant_lookup_value(
                        props, "Address", G_VARIANT_TYPE_STRING);

            if (addr_variant)
            {
                const gchar *addr = g_variant_get_string(addr_variant, NULL);

                if (g_ascii_strcasecmp(addr, g_cfg.target_mac) == 0)
                {
                    LOG_INFO("找到目标 ESP32: %s (%s)", addr, obj_path);
                    process_advertising_data(props);
                    watch_device(connection, obj_path);

                    /* 第一次命中的话启动定时轮询 ManufacturerData */
                    if (g_dev_path[0] == '\0')
                    {
                        g_dev_conn = connection;
                        strncpy(g_dev_path, obj_path, sizeof(g_dev_path) - 1);
                        if (s_poll_src == 0)
                            s_poll_src = g_timeout_add(500, poll_manufacturer_data, NULL);
                        LOG_INFO("启动 ManufacturerData 轮询 (500ms)");
                    }
                }

                g_variant_unref(addr_variant);
            }
        }
    }

    g_variant_unref(interfaces);
}

/**
  * @brief  检查 BlueZ 已缓存的设备
  * @retval TRUE  找到并开始监听
  *         FALSE 未找到
  */
static gboolean check_existing_devices(GDBusConnection *conn)
{
    GError *error = NULL;

    GDBusProxy *om_proxy = g_dbus_proxy_new_sync(
                conn, G_DBUS_PROXY_FLAGS_NONE,
                NULL, "org.bluez",
                "/", "org.freedesktop.DBus.ObjectManager",
                NULL, &error);

    if (error)
    {
        LOG_ERROR("ObjectManager 代理失败: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    GVariant *result = g_dbus_proxy_call_sync(
                om_proxy, "GetManagedObjects",
                NULL, G_DBUS_CALL_FLAGS_NONE,
                5000, NULL, &error);

    g_object_unref(om_proxy);

    if (error)
    {
        LOG_ERROR("GetManagedObjects 失败: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    GVariantIter *objects_iter;
    g_variant_get(result, "(a{oa{sa{sv}}})", &objects_iter);

    gchar       *obj_path;
    GVariantIter *interfaces_iter;
    gboolean     found = FALSE;

    while (g_variant_iter_loop(objects_iter, "{oa{sa{sv}}}",
                               &obj_path, &interfaces_iter))
    {
        gchar    *iface_name;
        GVariant *props;

        while (g_variant_iter_loop(interfaces_iter, "{s@a{sv}}",
                                   &iface_name, &props))
        {
            if (g_strcmp0(iface_name, "org.bluez.Device1") == 0)
            {
                GVariant *addr_variant = g_variant_lookup_value(
                            props, "Address", G_VARIANT_TYPE_STRING);

                if (addr_variant)
                {
                    const gchar *addr = g_variant_get_string(addr_variant, NULL);

                    if (g_ascii_strcasecmp(addr, g_cfg.target_mac) == 0)
                    {
                        LOG_INFO("在缓存中找到目标: %s", obj_path);
                        process_advertising_data(props);
                        watch_device(conn, obj_path);

                        if (g_dev_path[0] == '\0')
                        {
                            g_dev_conn = conn;
                            strncpy(g_dev_path, obj_path, sizeof(g_dev_path) - 1);
                            if (s_poll_src == 0)
                                s_poll_src = g_timeout_add(500, poll_manufacturer_data, NULL);
                            LOG_INFO("启动 ManufacturerData 轮询 (500ms)");
                        }
                        found = TRUE;
                    }

                    g_variant_unref(addr_variant);
                }
            }
        }

        if (found) break;
    }

    g_variant_iter_free(objects_iter);
    g_variant_unref(result);

    return found;
}

/**
  * @brief  直接操作 HCI 复位控制器，绕过 BlueZ D-Bus
  * @note   当 BlueZ 全部 D-Bus 手段（Stop/Start/Powered）都无效时，
  *         通过 hcitool 发 HCI_Reset 从硬件层复位，等价手动 hcitool lescan
  */
static void hci_reset_controller(void)
{
    /* hcitool 发原始 HCI 命令需要 root 权限（CAP_NET_RAW），
     * 网关必须以 sudo 运行才能走这条路径 */
    int ret = system("timeout 5 /usr/bin/hcitool cmd 0x03 0x0003 > /dev/null 2>&1");
    if (ret == 0)
    {
        LOG_WARN("[POLL] HCI 控制器已复位（原始 HCI 命令），等待重初始化");
        sleep(3);
    }
    else
    {
        LOG_WARN("[POLL] HCI 复位失败（hcitool 不可用或权限不足，试 sudo ./isk_gateway）");
    }
}

/**
  * @brief  执行一轮 discovery 启动：Stop → Filter → Start
  * @retval TRUE  启动成功
  *         FALSE 失败（含 InProgress：控制器残留扫描状态）
  */
static gboolean start_discovery_once(GDBusProxy *adapter)
{
    GVariant *result;
    GError   *error = NULL;

    /* SetDiscoveryFilter：纯 LE */
    {
        GVariant *filter = g_variant_new_parsed(
            "{'Transport': <'le'>}");
        result = g_dbus_proxy_call_sync(
            adapter, "SetDiscoveryFilter",
            g_variant_new_tuple(&filter, 1),
            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);

        if (error)
        {
            LOG_WARN("SetDiscoveryFilter 失败: %s (继续)", error->message);
            g_clear_error(&error);
        }
        else
        {
            LOG_INFO("BLE 扫描过滤已设置: Transport=le");
            g_variant_unref(result);
        }
    }

    /* StartDiscovery */
    result = g_dbus_proxy_call_sync(
                adapter, "StartDiscovery",
                NULL, G_DBUS_CALL_FLAGS_NONE,
                10000, NULL, &error);

    if (error)
    {
        LOG_ERROR("StartDiscovery 失败: %s", error->message);
        g_error_free(error);
        return FALSE;
    }
    LOG_INFO("BLE discovery 已启动");
    g_variant_unref(result);
    return TRUE;
}

/**
  * @brief  启动/重启 BLE discovery，失败自动升级
  * @note   控制器扫描状态卡死（InProgress）时 Stop/Start 救不回来，
  *         用原始 HCI 复位控制器（hcitool HCI_Reset），
  *         仍失败则重启 bluetoothd 兜底
  */
static void restart_discovery(GDBusConnection *conn)
{
    GError     *error = NULL;
    GVariant   *result;

    /* 创建适配器代理 */
    GDBusProxy *adapter = g_dbus_proxy_new_sync(
                conn, G_DBUS_PROXY_FLAGS_NONE,
                NULL, "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                NULL, &error);

    if (error)
    {
        LOG_ERROR("创建适配器代理失败: %s", error->message);
        g_error_free(error);
        return;
    }
    LOG_INFO("BLE 适配器就绪");

    /* 第 1 轮：Filter + Start（不 Stop，首次启动无需清残留） */
    if (start_discovery_once(adapter))
    {
        g_object_unref(adapter);
        return;
    }

    /* 失败 → 先尝试 Stop 清残留扫描（此时 Stop 才真正有意义） */
    result = g_dbus_proxy_call_sync(
                adapter, "StopDiscovery",
                NULL, G_DBUS_CALL_FLAGS_NONE,
                3000, NULL, NULL);
    if (result)
        g_variant_unref(result);
    usleep(200000);

    /* 深度复位（HCI / bluetoothd）受冷却保护：5 分钟一次，
     * 防止看门狗反复触发时把控制器/系统搞崩 */
    if (g_get_monotonic_time() / 1000 - s_last_deep_reset_ms < DEEP_RESET_COOLDOWN_MS)
    {
        gint64 remain = (DEEP_RESET_COOLDOWN_MS
                         - (g_get_monotonic_time() / 1000 - s_last_deep_reset_ms)) / 1000;
        LOG_WARN("[POLL] 深度复位冷却中（%llds 后可用），仅重试 Stop+Start",
                 (long long)remain);
        goto deep_reset_cooldown;
    }
    s_last_deep_reset_ms = g_get_monotonic_time() / 1000;

    /* 第 2 级：原始 HCI 复位控制器（绕过 BlueZ，等价手动 hcitool，
     * 实测有效；D-Bus 的 Powered off/on 在控制器卡死时永远 Busy 无效） */
    LOG_WARN("[POLL] 首次启动失败，HCI 直接复位控制器");
    hci_reset_controller();

    g_object_unref(adapter);
    adapter = g_dbus_proxy_new_sync(
                conn, G_DBUS_PROXY_FLAGS_NONE,
                NULL, "org.bluez",
                "/org/bluez/hci0",
                "org.bluez.Adapter1",
                NULL, NULL);
    if (!adapter)
    {
        LOG_ERROR("[POLL] HCI 复位后重建适配器代理失败");
        return;
    }

    if (!start_discovery_once(adapter))
    {
        LOG_ERROR("[POLL] HCI 复位后仍失败，重启 bluetoothd");

        /* 第 3 级：重启 bluetoothd，清软件层残留状态。
         * 不依赖 system() 返回码：bluetoothd 可能重启成功但 systemctl
         * 被 timeout 打断返回非零（曾误判"失败"）。执行后无条件等待重试 */
        system("timeout 20 systemctl restart bluetooth");
        sleep(5);   /* 等 bluetoothd 完全启动 + 控制器初始化 */

        g_object_unref(adapter);
        adapter = g_dbus_proxy_new_sync(
                    conn, G_DBUS_PROXY_FLAGS_NONE,
                    NULL, "org.bluez",
                    "/org/bluez/hci0",
                    "org.bluez.Adapter1",
                    NULL, NULL);
        if (!adapter)
        {
            LOG_ERROR("[POLL] bluetoothd 重启后重建适配器代理失败");
            return;
        }

        if (!start_discovery_once(adapter))
            LOG_ERROR("[POLL] 重启 bluetoothd 后仍无法启动 discovery");
    }

deep_reset_cooldown:

    /* 最终兜底：所有恢复均未成功开启新扫描，但如果 BlueZ 已有扫描在跑就直接复用 */
    {
        GError *fallback_err = NULL;
        result = g_dbus_proxy_call_sync(
                    adapter, "StartDiscovery",
                    NULL, G_DBUS_CALL_FLAGS_NONE,
                    5000, NULL, &fallback_err);
        if (fallback_err)
        {
            if (strstr(fallback_err->message, "InProgress"))
                LOG_INFO("[POLL] 恢复未生效，但 BlueZ 已有扫描，直接复用");
            else
                LOG_ERROR("[POLL] 所有恢复手段均失败: %s", fallback_err->message);
            g_error_free(fallback_err);
        }
        else
        {
            LOG_INFO("BLE discovery 已启动（兜底）");
        }
        if (result)
            g_variant_unref(result);
    }

    g_object_unref(adapter);
}

/* ================================================================
 *  公开 API
 * ================================================================ */

/**
  * @brief  启动 BLE 广播接收
  *
  * 只扫描、不连接。数据通过广播包的 ManufacturerData 获取。
  */
void ble_start(GDBusConnection *conn)
{
    if (geteuid() != 0)
        LOG_WARN("非 root 运行，D-Bus 复位和 HCI 恢复将失效，建议 sudo ./isk_gateway");

    /* 监听新设备出现：订阅挂在 D-Bus 连接上，重启 discovery 不会失效 */
    g_dbus_connection_signal_subscribe(
        conn,
        "org.bluez",
        "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded",
        NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_added, NULL, NULL);

    /* 启动 discovery + 检查已知设备（防止 ESP32 在 Pi 启动前已经在广播） */
    restart_discovery(conn);
    check_existing_devices(conn);
}
