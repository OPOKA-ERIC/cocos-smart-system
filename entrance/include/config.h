#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Door feedback
#define DOOR_LOCK_GPIO      GPIO_NUM_2
#define BUZZER_GPIO         GPIO_NUM_18

// SD card - dedicated SPI bus (SPI3)
#define SD_CS_GPIO          GPIO_NUM_10
#define SD_MOSI_GPIO        GPIO_NUM_11
#define SD_MISO_GPIO        GPIO_NUM_13
#define SD_CLK_GPIO         GPIO_NUM_12

// MFRC522 RFID readers - shared SPI bus (SPI2), separate chip selects
#define RFID_MOSI_GPIO      GPIO_NUM_6
#define RFID_MISO_GPIO      GPIO_NUM_7
#define RFID_SCK_GPIO       GPIO_NUM_15
#define RFID_ENTRY_CS_GPIO  GPIO_NUM_5
#define RFID_ENTRY_RST_GPIO GPIO_NUM_4
#define RFID_EXIT_CS_GPIO   GPIO_NUM_16
#define RFID_EXIT_RST_GPIO  GPIO_NUM_17

// Wi-Fi (Wokwi internet gateway)
#define WIFI_SSID           "Wokwi-GUEST"
#define WIFI_PASS           ""

// MQTT
#define MQTT_HOST           "broker.hivemq.com"
#define MQTT_PORT           1883
#define MQTT_TOPIC_PREFIX   "cocis/2026/opoka-eric"
#define MQTT_CLIENT_ID      "cocis-entrance-opoka"
#define MQTT_STATUS_TOPIC   MQTT_TOPIC_PREFIX "/status/entrance"
#define TOPIC_ENTRANCE_EVENT MQTT_TOPIC_PREFIX "/entrance/event"
#define TOPIC_ENTRANCE_STATE MQTT_TOPIC_PREFIX "/entrance/state"
#define TOPIC_ROOM1_STATE   MQTT_TOPIC_PREFIX "/room1/state"
#define TOPIC_ROOM2_STATE   MQTT_TOPIC_PREFIX "/room2/state"
#define TOPIC_ROOM1_STATUS  MQTT_TOPIC_PREFIX "/status/room1"
#define TOPIC_ROOM2_STATUS  MQTT_TOPIC_PREFIX "/status/room2"
#define TOPIC_ROOM1_CMD     MQTT_TOPIC_PREFIX "/room1/cmd"
#define TOPIC_ROOM2_CMD     MQTT_TOPIC_PREFIX "/room2/cmd"

// Access policy
#define ACCESS_START_HOUR_DEFAULT 7
#define ACCESS_END_HOUR_DEFAULT   21
#define MAX_PEOPLE                50
#define ADMIN_PIN_DEFAULT         "1234"
#define AUTH_WINDOW_MS            60000

// Storage
#define SD_MOUNT_POINT      "/sdcard"
#define SD_LOG_FILE         SD_MOUNT_POINT "/access.log"
#define NVS_NAMESPACE       "cocis"

#endif
