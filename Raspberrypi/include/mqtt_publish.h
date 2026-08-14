/**
  ******************************************************************************
  * @file    mqtt_publish.h
  * @brief   帧→MQTT 桥接：ParsedFrame_t → JSON → mosquitto_publish
  ******************************************************************************
  */
#ifndef MQTT_PUBLISH_H
#define MQTT_PUBLISH_H

#include <mosquitto.h>
#include "comm_parse.h"

/* MQTT Topic 定义
 * ThingsBoard 遥测 topic：三类帧统一发到这一个 topic，靠 JSON 里的 type 字段区分。
 * 原 topic（fall_detection/pi01/data|alert|status）已随迁移废弃。 */
#define TOPIC_DATA   "v1/devices/me/telemetry"
#define TOPIC_ALERT  "v1/devices/me/telemetry"
#define TOPIC_STATUS "v1/devices/me/telemetry"

/**
  * @brief  将解析后的帧转 JSON 并发布到对应的 MQTT topic
  * @param  mosq   mosquitto 实例
  * @param  frame  已解析的帧
  * @retval 0  成功
  *         -1 失败（内存/发布错误）
  */
int mqtt_publish_frame(struct mosquitto *mosq, const ParsedFrame_t *frame);

#endif /* MQTT_PUBLISH_H */
