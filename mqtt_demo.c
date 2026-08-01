#include <mosquitto.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>

#define MQTT_HOST	"121.40.252.238"
#define MQTT_PORT	1883
#define MQTT_USERNAME	"flyzzz"
#define MQTT_PASSWORD	"chanhjf17"
#define DEVICE_ID	"pi01"


#define TOPIC_DATA	"fall_detection/" DEVICE_ID "/data"
#define TOPIC_ALERT 	"fall_detection/" DEVICE_ID "/alert"
#define TOPIC_STATUS	"fall_detection/" DEVICE_ID "/status"
#define TOPIC_CMD	"fall_detection/" DEVICE_ID "/cmd"

static void on_connect(struct mosquitto *mosq, void *userdata, int rc)
{
	if(rc == 0)
	{
		printf("[MQTT] 连接成功\n");
		mosquitto_subscribe(mosq, NULL, TOPIC_CMD, 1);
		mosquitto_publish(mosq, NULL, TOPIC_STATUS, strlen("online"),
			       	"online", 0, false);
	}
	else
	{
		printf("[MQTT] 连接失败，rc=%d (%s)\n", rc, 
				mosquitto_connack_string(rc));
	}
}

static void on_message(struct mosquitto *mosq, void *userdata,  
			const struct mosquitto_message *msg)
{
	printf("[MQTT] 收到消息 topic=%s payload=%.*s\n", 
			msg->topic, msg->payloadlen, (char *)msg->payload);
	cJSON *root = cJSON_ParseWithLength((const char *)msg->payload, msg->payloadlen);
	if(root == NULL)
	{
		printf("[MQTT] 指令JSON解析失败\n");
		return ;
	}

	cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
	if(cJSON_IsString(cmd))
	{
		printf("[CMD] 收到指令：%s\n", cmd->valuestring);
		/*TODO:*/
	}

	cJSON_Delete(root);
}

static void publish_data(struct mosquitto *mosq)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "device_id", DEVICE_ID);
	cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));

	cJSON *accel = cJSON_CreateObject();
	cJSON_AddNumberToObject(accel, "x", 0.02);
	cJSON_AddNumberToObject(accel, "y", 9.81);
	cJSON_AddNumberToObject(accel, "z", 0.15);
	cJSON_AddItemToObject(root, "accel", accel);

	cJSON_AddStringToObject(root, "gesture", "standing");

	char *payload = cJSON_PrintUnformatted(root);
	int  rc = mosquitto_publish(mosq, NULL, TOPIC_DATA,
				    (int)strlen(payload), payload, 0, false);
	if(rc != MOSQ_ERR_SUCCESS)
	{
		printf("[MQTT] 发布失败: %s\n", mosquitto_strerror(rc));
	}
	else
	{
		printf("[MQTT] 已发布: %s\n", payload);
	}

	free(payload);
	cJSON_Delete(root);
}

int main(void)
{
	mosquitto_lib_init();

	struct mosquitto *mosq = mosquitto_new(DEVICE_ID, true, NULL);
	if(!mosq)
	{
		fprintf(stderr, "创建 mosquitto 实例失败\n");
		return 1;
	}

	mosquitto_username_pw_set(mosq, MQTT_USERNAME, MQTT_PASSWORD);
	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_message_callback_set(mosq, on_message);

	int rc = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);
	if(rc != MOSQ_ERR_SUCCESS)
	{
		fprintf(stderr, "连接失败: %s\n", mosquitto_strerror(rc));
		return 2;
	}

	mosquitto_loop_start(mosq);

	while(1)
	{
		publish_data(mosq);;
		sleep(5);
	}

	mosquitto_loop_stop(mosq, true);
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();

	return 0;
}
