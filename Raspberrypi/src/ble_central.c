/**
  ******************************************************************************
  * @file    ble_central.c
  * @brief   BLE 广播接收 — 从 ESP32 广播包 ManufacturerData 中提取帧
  *
  * 工作方式（纯扫描，不连接）：
  *   1. 启动 BLE 扫描（StartDiscovery）
  *   2. 发现设备 → 读 ManufacturerData → 解析帧 → MQTT
  *   3. 订阅 PropertiesChanged → 每次 ESP32 更新广播数据，立即收到新帧
  *
  * 注意：ESP32 需要把帧数据写入 BLE 广播包 Manufacturer Specific Data 字段。
  ******************************************************************************
  */
#include <gio/gio.h>
#include <stdio.h>
#include "comm_parse.h"
#include "mqtt_publish.h"
#include "log.h"
#include "config.h"

/* MQTT 实例（main.c 中初始化） */
extern struct mosquitto *g_mosq;

/* ================================================================
 *  内部辅助
 * ================================================================ */

/**
  * @brief  从设备属性中读取 ManufacturerData 并尝试解析帧
  *
  * BlueZ 把广播包中的 Manufacturer Specific Data（AD type 0xFF）
  * 映射为 Device1 接口的 ManufacturerData 属性，类型 a{qv}：
  *   - key:  uint16 厂商 ID
  *   - value: variant → byte array（即原始帧数据）
  */
static void process_advertising_data(GVariant *props)
{
    GVariant    *mfg_data;
    GVariantIter iter;
    guint16      mfg_id;
    GVariant    *value;

    mfg_data = g_variant_lookup_value(props, "ManufacturerData",
                                      G_VARIANT_TYPE("a{qv}"));
    if (mfg_data == NULL)
        return;

    g_variant_iter_init(&iter, mfg_data);

    while (g_variant_iter_next(&iter, "{qv}", &mfg_id, &value))
    {
        gsize        len;
        const guint8 *data;
        ParsedFrame_t frame;

        data = g_variant_get_fixed_array(value, &len, sizeof(guint8));

        LOG_INFO("[ADV] mfg_id=0x%04X, len=%zu", mfg_id, len);

        if (comm_parse_frame(data, len, &frame))
        {
            LOG_INFO("[ADV] 解析帧成功 type=%02X", frame.type);

            if (g_mosq)
                mqtt_publish_frame(g_mosq, &frame);
        }
        else
        {
            LOG_INFO("[ADV] 未识别的数据：%zu 字节", len);
        }

        g_variant_unref(value);
    }

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
                LOG_INFO("发现设备: %s (%s)", addr, obj_path);

                if (g_ascii_strcasecmp(addr, g_cfg.target_mac) == 0)
                {
                    LOG_INFO("找到 ESP32，开始接收广播数据");
                    process_advertising_data(props);
                    watch_device(connection, obj_path);
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
                -1, NULL, &error);

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
    GError   *error  = NULL;
    GVariant *result = NULL;

    /* 1. 获取适配器代理 */
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

    /* 2. 监听新设备出现 */
    g_dbus_connection_signal_subscribe(
        conn,
        "org.bluez",
        "org.freedesktop.DBus.ObjectManager",
        "InterfacesAdded",
        NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_interfaces_added, NULL, NULL);

    /* 2.5 确保没残留扫描，然后设过滤：纯 LE + 不丢重复包 */
    {
        /* 先停掉可能的旧扫描（忽略返回/错误） */
        g_dbus_proxy_call_sync(adapter, "StopDiscovery",
            NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);

        GVariant *filter = g_variant_new_parsed(
            "{'Transport': <'le'>, 'DuplicateData': <false>}");
        result = g_dbus_proxy_call_sync(
            adapter, "SetDiscoveryFilter",
            g_variant_new_tuple(&filter, 1),
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

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

    /* 3. 启动扫描 */
    result = g_dbus_proxy_call_sync(
                adapter, "StartDiscovery",
                NULL, G_DBUS_CALL_FLAGS_NONE,
                10000, NULL, &error);

    if (error)
    {
        LOG_ERROR("StartDiscovery 失败: %s", error->message);
        g_error_free(error);
        g_object_unref(adapter);
        return;
    }
    LOG_INFO("BLE 扫描已启动（纯广播模式）");

    /* 4. 检查已知设备（防止 ESP32 在 Pi 启动前已经在广播） */
    check_existing_devices(conn);

    g_variant_unref(result);
    g_object_unref(adapter);
}
