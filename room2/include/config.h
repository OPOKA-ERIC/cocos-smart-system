#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Room config
#define ROOM_TAG "ROOM2"
#define PIR_GPIO GPIO_NUM_4
#define LIGHT_GPIO GPIO_NUM_2

// Wi-Fi / MQTT
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""
#define MQTT_BROKER_URI "mqtt://broker.hivemq.com"
#define MQTT_CLIENT_ID "cocis-room2"

#endif
