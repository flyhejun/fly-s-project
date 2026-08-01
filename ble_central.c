#include <gio/gio.h>
#include <stdio.h>

#define TARGET_MAC "58:8c:81:0e:4e:16"

#define TARGET_CHAR_UUID "0000c305-0000-1000-8000-00805f9b34fb"

static char *g_device_path = NULL;

static void handle_notify_data(const guint8 *data, gsize len)
{
	printf("[数据] 收到 %zu 字节: ", len);
	for(gsize i=0; i<len; i++)
	{
		printf("%02X ", data[i]);
	}
	printf("\n");
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

	if(g_strcmp0(iface, "org.bluez.GattCharacteristic1") == 0)
	{
		GVariant *value = g_variant_lookup_value(changed_props, "Value", 
							G_VARIANT_TYPE("ay"));
		if(value)
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

	if(error)
	{
		fprintf(stderr, "创建特征值代理失败: %s\n", error->message);
		g_error_free(error);
		return ;
	}

	g_dbus_connection_signal_subscribe(conn, "org.bluez",
				      "org.freedesktop.DBus.Properties","PropertiesChanged",
					char_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					on_char_properties_changed, NULL, NULL);

	GVariant *result = g_dbus_proxy_call_sync(char_proxy, "StartNotify", NULL,
						  G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

	if(error)
	{
		fprintf(stderr, "StartNotify失败: %s\n", error->message);
		g_error_free(error);
	}
	else
	{
		printf("已订阅NOtify: %s\n", char_path);
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
	if(error)
	{	
		fprintf(stderr, "创建ObjectManager代理失败: %s\n", error->message);
		g_error_free(error);
		return;
	}

	GVariant *result = g_dbus_proxy_call_sync(om_proxy, "GetManagedObjects", NULL,
						  G_DBUS_CALL_FLAGS_NONE, -1,
						  NULL, &error);
	g_object_unref(om_proxy);

	if(error)
	{
		fprintf(stderr, "GetManagedObjects失败: %s\n", error->message);
		g_error_free(error);
		return ;
	}

	GVariantIter *objects_iter;
	g_variant_get(result, "(a{oa{sa{sv}}})", &objects_iter);

	gchar 	     *obj_path;
	GVariantIter *interfaces_iter;
	gboolean     found = FALSE;

	while(g_variant_iter_loop(objects_iter, "{oa{sa{sv}}}", &obj_path, &interfaces_iter))
	{
		gchar *iface_name;
		GVariant *props;

		while(g_variant_iter_loop(interfaces_iter, "{s@a{sv}}", &iface_name, &props))
		{
			if(g_strcmp0(iface_name, "org.bluez.GattCharacteristic1") == 0 &&
		   	g_device_path != NULL && g_str_has_prefix(obj_path, g_device_path))
			{	
				GVariant *uuid_variant = g_variant_lookup_value(props, "UUID",
									G_VARIANT_TYPE_STRING);
				if(uuid_variant)
				{
					const gchar *uuid = g_variant_get_string(uuid_variant, NULL);
					if(g_strcmp0(uuid, TARGET_CHAR_UUID) == 0)
					{
						printf(">>> 找到目标特征值: %s\n", obj_path);
						start_notify(conn, obj_path);
						found = TRUE;
					}
	
					g_variant_unref(uuid_variant);
				}
			}
		}
		if(found) break;
	}

	g_variant_iter_free(objects_iter);
	g_variant_unref(result);

	if(!found)
	{
		fprintf(stderr, "没找到UUID=%s的特征值\n", TARGET_CHAR_UUID);
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

	if(g_strcmp0(iface, "org.bluez.Device1") == 0)
	{
		GVariant *resolved = g_variant_lookup_value(changed_props, "ServicesResolved",
							    G_VARIANT_TYPE_BOOLEAN);
		if(resolved)
		{
			if(g_variant_get_boolean(resolved))
			{
				printf("服务发现完成！ServicesResolved = true\n");
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
        
	if(error)
	{
		fprintf(stderr, "创建设备代理失败: %s\n", error->message);
		g_error_free(error);
		return ;
	}

	g_dbus_connection_signal_subscribe(conn, "org.bluez","org.freedesktop.DBus.Properties",
					   "PropertiesChanged", device_path,
					   NULL, G_DBUS_SIGNAL_FLAGS_NONE,
					   on_device_properties_changed, NULL, NULL);
		
	GVariant *result = g_dbus_proxy_call_sync(
			dev_proxy, "Connect",
			NULL, G_DBUS_CALL_FLAGS_NONE,
			15000, NULL, &error);

	if(error)
	{
		fprintf(stderr, "Connect失败: %s\n", error->message);
		g_error_free(error);
	}
	else
	{
		printf("Connect() 调用成功\n");
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

	if(error)
	{
		fprintf(stderr, "创建ObjectManager代理失败: %s\n", error->message);
		g_error_free(error);
		return FALSE;
	}

	GVariant *result = g_dbus_proxy_call_sync(
			om_proxy, "GetManagedObjects",
			NULL, G_DBUS_CALL_FLAGS_NONE,
			-1, NULL, &error);

	g_object_unref(om_proxy);

	if(error)
	{
		fprintf(stderr, "Get Managed Objects失败: %s\n", error->message);
		g_error_free(error);
		return FALSE;
	}

	GVariantIter *objects_iter;
	g_variant_get(result, "(a{oa{sa{sv}}})", &objects_iter);

	gchar *obj_path;
	GVariantIter *interfaces_iter;
	gboolean found = FALSE;

	while(g_variant_iter_loop(objects_iter, "{oa{sa{sv}}}", &obj_path, &interfaces_iter))
	{
		gchar *iface_name;
		GVariant* props;

		while(g_variant_iter_loop(interfaces_iter, "{s@a{sv}}", &iface_name, &props))
		{
			if(g_strcmp0(iface_name, "org.bluez.Device1") == 0)
			{
				GVariant *addr_variant = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
				if(addr_variant)
				{
					const gchar *addr = g_variant_get_string(addr_variant, NULL);
					printf("已知设备: %s (%s)\n", addr, obj_path);

					if(g_ascii_strcasecmp(addr, TARGET_MAC) == 0)
					{
						printf(">>> 在已知设备中找到目标，开始连接: %s\n", obj_path);
						connect_to_device(conn, obj_path);
						found = TRUE;
					}

					g_variant_unref(addr_variant);
				}
			}
		}
		if(found) break;
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

	while(g_variant_iter_loop(&iter, "{s@a{sv}}", &iface_name, &props))
	{
		if(g_strcmp0(iface_name, "org.bluez.Device1") == 0)
		{
			GVariant *addr_variant = g_variant_lookup_value(props, "Address", G_VARIANT_TYPE_STRING);
			if(addr_variant)
			{
				const gchar *addr = g_variant_get_string(addr_variant, NULL);
				printf("发现设备: %s (%s)\n", addr, obj_path);

				if(g_ascii_strcasecmp(addr, TARGET_MAC) == 0)
				{
					printf(">>> 找到目标设备！准备连接: %s\n", obj_path);
					connect_to_device(connection, obj_path);
				}

				g_variant_unref(addr_variant);
			}
		}
	}
	
	g_variant_unref(interfaces);
}

int main(void)
{
	GError *error = NULL;
	GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);

	if(error)
	{
		fprintf(stderr, "连接失败: %s\n", error->message);
		g_error_free(error);
		return -1;
	}

	printf("成功连接到 System Bus\n");
	
	GDBusProxy *adapter = g_dbus_proxy_new_sync(
			conn,
			G_DBUS_PROXY_FLAGS_NONE,
			NULL,
			"org.bluez",
			"/org/bluez/hci0",
			"org.bluez.Adapter1",
			NULL,
			&error);

	if(error)
	{
		fprintf(stderr, "创建适配器代理失败: %s\n", error->message);
		g_error_free(error);
		return -2;
	}
	printf("适配器代理创建成功\n");
	
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

	if(error)
	{
		fprintf(stderr, "StartDiscovery失败\n", error->message);
		g_error_free(error);
		return -3;
	}
	printf("开始扫描\n");

	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	printf("进入主循环，等待设备发现(CTRL+C 退出)...\n");

	check_existing_devices(conn);

	g_main_loop_run(loop);

	g_variant_unref(result);
	g_object_unref(conn);
	return 0;

}
