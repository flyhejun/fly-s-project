#include <gio/gio.h>
#include <stdio.h>
#include "comm_parse.h"
#include "mqtt_publish.h"
#include "log.h"

/* MQTT 实例（main.c 初始化后赋值，Notify 回调中使用） */
extern struct mosquitto *g_mosq;

#define TARGET_MAC "58:8c:81:0e:4e:16"

#define TARGET_CHAR_UUID "0000a003-0000-1000-8000-00805f9b34fb"

static char *g_device_path = NULL;

static void handle_notify_data(const guint8 *data, gsize len)
{
	ParsedFrame_t frame;

	if (comm_parse_frame(data, len, &frame))
	{
		LOG_INFO("[BLE] 收到帧 type=%02X, date=%04u-%02u-%02u %02u:%02u",
		         frame.type, frame.date.year, frame.date.month,
		         frame.date.day, frame.date.hour, frame.date.minute);

		if (g_mosq)
			mqtt_publish_frame(g_mosq, &frame);
	}
	else
	{
		LOG_INFO("[BLE] 未识别的数据: %.*s", (int)len, (const char *)data);
	}
}

static void on_char_properties_changed(GDBusConnection *connection,
					const gchar *sender_name,
					const gchar *object_path,
					const gchar *interface_name,
					const gchar *signal_name,
					GVariant *parameters,
					gpointer user_data)
{
	const gchar *iface;
	GVariant *changed_props;
	GVariant *invalidated_props;

	g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed_props, &invalidated_props);

	if (g_strcmp0(iface, "org.bluez.GattCharacteristic1") == 0)
	{
		GVariant *value = g_variant_lookup_value(changed_props, "Value",
							G_VARIANT_TYPE("ay"));
		if (value)
		{
			gsize len;
			const guint8 *data = g_variant_get_fixed_array(value, &len,
									sizeof(guint8));
			handle_notify_data(data, len);
			g_variant_unref(value);
		}
	}

	g_variant_unref(changed_props);
	g_variant_unref(invalidated_props);
}

static void start_notify(GDBusConnection *conn, const gchar *char_path)
{
	GError *error = NULL;

	GDBusProxy *char_proxy = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
						"org.bluez", char_path,
						"org.bluez.GattCharacteristic1", NULL,
						&error);

	if (error)
	{
		LOG_ERROR("创建特征值代理失败: %s", error->message);
		g_error_free(error);
		return;
	}

	g_dbus_connection_signal_subscribe(conn, "org.bluez",
				      "org.freedesktop.DBus.Properties","PropertiesChanged",
					char_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					on_char_properties_changed, NULL, NULL);

	GVariant *result = g_dbus_proxy_call_sync(char_proxy, "StartNotify", NULL,
						  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

	if (error)
	{
		LOG_ERROR("StartNotify 失败: %s", error->message);
		g_error_free(error);
	}
	else
	{
		LOG_INFO("已订阅 Notify: %s", char_path);
		g_variant_unref(result);
	}
}

static void find_target_characteristic(GDBusConnection *conn)
{
	GError *error = NULL;
	GDBusProxy *om_proxy = g_dbus_proxy_new_sync(conn, G_DBUS_PROXY_FLAGS_NONE, NULL,
						     "org.bluez", "/",
						     "org.freedesktop.DBus.ObjectManager",
						     NULL, &error);
	if (error)
	{
		LOG_ERROR("创建 ObjectManager 代理失败: %s", error->message);
		g_error_free(error);
		return;
	}

	GVariant *result = g_dbus_proxy_call_sync(om_proxy, "GetManagedObjects", NULL,
						  G_DBUS_CALL_FLAGS_NONE, -1,
						  NULL, &error);
	g_object_unref(om_proxy);

	if (error)
	{
		LOG_ERROR("GetManagedObjects 失败: %s", error->message);
		g_error_free(error);
		return;
	}

	GVariantIter *objects_iter;
	g_variant_get(result, "(a{oa{sa{sv}}})", &objects_iter);

	gchar        *obj_path;
	GVariantIter *interfaces_iter;
	gboolean     found = FALSE;

	while (g_variant_iter_loop(objects_iter, "{oa{sa{sv}}}", &obj_path, &interfaces_iter))
	{
		gchar *iface_name;
		GVariant *props;

		while (g_variant_iter_loop(interfaces_iter, "{s@a{sv}}", &iface_name, &props))
		{
			if (g_strcmp0(iface_name, "org.bluez.GattCharacteristic1") == 0 &&
			    g_device_path != NULL && g_str_has_prefix(obj_path, g_device_path))
			{
				GVariant *uuid_variant = g_variant_lookup_value(props, "UUID",
									G_VARIANT_TYPE_STRING);
				if (uuid_variant)
				{
					const gchar *uuid = g_variant_get_string(uuid_variant, NULL);
					if (g_strcmp0(uuid, TARGET_CHAR_UUID) == 0)
					{
						LOG_INFO("找到目标特征值: %s", obj_path);
						start_notify(conn, obj_path);
						found = TRUE;
					}

					g_variant_unref(uuid_variant);
				}
			}
		}
		if (found) break;
	}

	g_variant_iter_free(objects_iter);
	g_variant_unref(result);

	if (!found)
	{
		LOG_WARN("没找到 UUID=%s 的特征值", TARGET_CHAR_UUID);
	}
}


static void on_device_properties_changed(GDBusConnection *connection,
					const gchar *sender_name,
					const gchar *object_path,
					const gchar *interface_name,
					const gchar *signal_name,
					GVariant *parameters,
					gpointer user_data)
{
	const gchar *iface;
	GVariant *changed_props;
	GVariant *invalidated_props;

	g_variant_get(parameters, "(&s@a{sv}@as)", &iface, &changed_props, &invalidated_props);

	if (g_strcmp0(iface, "org.bluez.Device1") == 0)
	{
		GVariant *resolved = g_variant_lookup_value(changed_props, "ServicesResolved",
							    G_VARIANT_TYPE_BOOLEAN);
		if (resolved)
		{
			if (g_variant_get_boolean(resolved))
			{
				LOG_INFO("服务发现完成，开始查找特征值");
				find_target_characteristic(connection);
			}

			g_variant_unref(resolved);
		}
	}

	g_variant_unref(changed_props);
	g_variant_unref(invalidated_props);
}

static void connect_to_device(GDBusConnection *conn, const gchar *device_path)
{
	GError *error = NULL;
	g_device_path = g_strdup(device_path);

	GDBusProxy *dev_proxy = g_dbus_proxy_new_sync(
				conn, G_DBUS_PROXY_FLAGS_NONE,
				NULL, "org.bluez",
				device_path, "org.bluez.Device1",
				NULL, &error);

	if (error)
	{
		LOG_ERROR("创建设备代理失败: %s", error->message);
		g_error_free(error);
		return;
	}

	g_dbus_connection_signal_subscribe(conn, "org.bluez","org.freedesktop.DBus.Properties",
					   "PropertiesChanged", device_path,
					   NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					   on_device_properties_changed, NULL, NULL);

	GVariant *result = g_dbus_proxy_call_sync(
			dev_proxy, "Connect",
			NULL, G_DBUS_CALL_FLAGS_NONE,
			15000, NULL, &error);

	if (error)
	{
		LOG_ERROR("Connect 失败: %s", error->message);
		g_error_free(error);
	}
	else
	{
		LOG_INFO("Connect() 调用成功");
		g_variant_unref(result);
	}

	g_object_unref(dev_proxy);
}

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
		LOG_ERROR("创建 ObjectManager 代理失败: %s", error->message);
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

	gchar *obj_path;
	GVariantIter *interfaces_iter;
	gboolean found = FALSE;

	while (g_variant_iter_loop(objects_iter, "{oa{sa{sv}}}", &obj_path, &interfaces_iter))
	{
		gchar *iface_name;
		GVariant* props;

		while (g_variant_iter_loop(interfaces_iter, "{s@a{sv}}", &iface_name, &props))
		{
			if (g_strcmp0(iface_name, "org.bluez.Device1") == 0)
			{
				GVariant *addr_variant = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
				if (addr_variant)
				{
					const gchar *addr = g_variant_get_string(addr_variant, NULL);
					LOG_INFO("已知设备: %s (%s)", addr, obj_path);

					if (g_ascii_strcasecmp(addr, TARGET_MAC) == 0)
					{
						LOG_INFO("在已知设备中找到目标，开始连接: %s", obj_path);
						connect_to_device(conn, obj_path);
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

static void on_interfaces_added(GDBusConnection *connection, const gchar *sender_name,
				const gchar *object_path, const gchar *interface_name,
				const gchar *signal_name, GVariant *parameters,
				gpointer user_data)
{
	const gchar *obj_path;
	GVariant *interfaces;

	g_variant_get(parameters, "(&o@a{sa{sv}})", &obj_path, &interfaces);

	GVariantIter iter;
	gchar *iface_name;
	GVariant *props;
	g_variant_iter_init(&iter, interfaces);

	while (g_variant_iter_loop(&iter, "{s@a{sv}}", &iface_name, &props))
	{
		if (g_strcmp0(iface_name, "org.bluez.Device1") == 0)
		{
			GVariant *addr_variant = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
			if (addr_variant)
			{
				const gchar *addr = g_variant_get_string(addr_variant, NULL);
				LOG_INFO("发现设备: %s (%s)", addr, obj_path);

				if (g_ascii_strcasecmp(addr, TARGET_MAC) == 0)
				{
					LOG_INFO("找到目标设备，准备连接: %s", obj_path);
					connect_to_device(connection, obj_path);
				}

				g_variant_unref(addr_variant);
			}
		}
	}

	g_variant_unref(interfaces);
}

void ble_start(GDBusConnection *conn)
{
	GError *error = NULL;

	GDBusProxy *adapter = g_dbus_proxy_new_sync(
			conn,
			G_DBUS_PROXY_FLAGS_NONE,
			NULL,
			"org.bluez",
			"/org/bluez/hci0",
			"org.bluez.Adapter1",
			NULL,
			&error);

	if (error)
	{
		LOG_ERROR("创建适配器代理失败: %s", error->message);
		g_error_free(error);
		return;
	}
	LOG_INFO("适配器代理创建成功");

	g_dbus_connection_signal_subscribe(
			conn,
			"org.bluez",
			"org.freedesktop.DBus.ObjectManager",
			"InterfacesAdded",
			NULL,
			NULL,
			G_DBUS_SIGNAL_FLAGS_NONE,
			on_interfaces_added,
			NULL,
			NULL);

	GVariant *result = g_dbus_proxy_call_sync(
			adapter,
			"StartDiscovery",
			NULL,
			G_DBUS_CALL_FLAGS_NONE,
			-1,
			NULL,
			&error);

	if (error)
	{
		LOG_ERROR("StartDiscovery 失败: %s", error->message);
		g_error_free(error);
		return;
	}
	LOG_INFO("开始扫描");

	check_existing_devices(conn);

	g_variant_unref(result);
	g_object_unref(adapter);
}
