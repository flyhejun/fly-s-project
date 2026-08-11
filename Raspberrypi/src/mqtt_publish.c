/**
  ******************************************************************************
  * @file    mqtt_publish.c
  * @brief   帧→MQTT 桥接实现
  ******************************************************************************
  */
#include "mqtt_publish.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "log.h"

/* 状态名查表 */
static const char *STATE_NAMES[] = {
    "NORMAL", "FREE_FALL", "IMPACT", "MOTIONLESS"
};

int mqtt_publish_frame(struct mosquitto *mosq, const ParsedFrame_t *frame)
{
    cJSON       *root;
    char        *payload;
    char         date_str[24];
    const char  *topic;
    int          rc;
    time_t       now = time(NULL);
    struct tm   *t = localtime(&now);

    if (mosq == NULL || frame == NULL)
        return -1;

    root = cJSON_CreateObject();
    if (root == NULL)
        return -1;

    /* 公共字段 */
    cJSON_AddStringToObject(root, "device_id", "pi01");

    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d %02d:%02d:%02d",
         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
         t->tm_hour, t->tm_min, t->tm_sec);

    switch (frame->type)
    {
        case FRAME_TYPE_NOTIFY:
            cJSON_AddStringToObject(root, "type", "event_notify");
            cJSON_AddStringToObject(root, "date", date_str);
            cJSON_AddStringToObject(root, "event_type", "Fall_detected");
            cJSON_AddNumberToObject(root, "accel_sq", frame->data.notify.accel_sq);
            cJSON_AddNumberToObject(root, "gyro_sq", frame->data.notify.gyro_sq);

            topic = TOPIC_ALERT;
            break;

        case FRAME_TYPE_REAL_TIME:
            cJSON_AddStringToObject(root, "type", "real_time");
            cJSON_AddStringToObject(root, "date", date_str);
            cJSON_AddNumberToObject(root, "accel_sq", frame->data.real_time.accel_sq);
            cJSON_AddNumberToObject(root, "gyro_sq", frame->data.real_time.gyro_sq);
         
            topic = TOPIC_DATA;
            break;

        case FRAME_TYPE_STATUS_REPLY:
            cJSON_AddStringToObject(root, "type", "status");
            cJSON_AddStringToObject(root, "state", STATE_NAMES[frame->data.state]);

            topic = TOPIC_STATUS;
            break;

        default:
            cJSON_Delete(root);
            return -1;
    }

    /* 序列化 + 发布 */
    payload = cJSON_PrintUnformatted(root);
    if (payload == NULL)
    {
        cJSON_Delete(root);
        return -1;
    }

    rc = mosquitto_publish(mosq, NULL, topic,
                           (int)strlen(payload), payload, 0, false);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        LOG_ERROR("[MQTT] 发布失败 (topic=%s): %s", topic, mosquitto_strerror(rc));
    }
    else
    {
        LOG_INFO("[MQTT] 已发布 (topic=%s): %s", topic, payload);
    }

    free(payload);
    cJSON_Delete(root);
    return (rc == MOSQ_ERR_SUCCESS) ? 0 : -1;
}
