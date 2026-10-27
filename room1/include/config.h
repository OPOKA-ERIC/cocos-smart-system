#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

#define ROOM_TAG        "ROOM1"
#define ROOM_NAME       "Room 1"
#define ROOM_TERM       "room1"

#define PIR_GPIO        GPIO_NUM_4
#define LIGHT_GPIO      GPIO_NUM_2
#define OCCUPANCY_TIMEOUT_MS 15000

#define WIFI_SSID       "Wokwi-GUEST"
#define WIFI_PASS       ""

#define MQTT_HOST       "172.28.167.62"
#define MQTT_PORT       1883
#define MQTT_TOPIC_PREFIX "cocis/2026/opoka-eric"
#define MQTT_CLIENT_ID  "cocis-room1-opoka"
#define MQTT_STATUS_TOPIC MQTT_TOPIC_PREFIX "/status/room1"
#define TOPIC_ROOM_STATE MQTT_TOPIC_PREFIX "/room1/state"
#define TOPIC_ROOM_CMD   MQTT_TOPIC_PREFIX "/room1/cmd"

#endif
