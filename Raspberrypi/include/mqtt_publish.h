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

/* MQTT Topic 定义 */
#define TOPIC_DATA   "fall_detection/pi01/data"
#define TOPIC_ALERT  "fall_detection/pi01/alert"
#define TOPIC_STATUS "fall_detection/pi01/status"

/**
  * @brief  将解析后的帧转 JSON 并发布到对应的 MQTT topic
  * @param  mosq   mosquitto 实例
  * @param  frame  已解析的帧
  * @retval 0  成功
  *         -1 失败（内存/发布错误）
  */
int mqtt_publish_frame(struct mosquitto *mosq, const ParsedFrame_t *frame);

#endif /* MQTT_PUBLISH_H */
