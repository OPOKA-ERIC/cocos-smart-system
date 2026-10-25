#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Door lock
#define DOOR_LOCK_GPIO GPIO_NUM_2

// SD Card SPI (VSPI)
#define SD_CS_GPIO GPIO_NUM_10
#define SD_MOSI_GPIO GPIO_NUM_11
#define SD_MISO_GPIO GPIO_NUM_13
#define SD_CLK_GPIO GPIO_NUM_12

// RFID MFRC522 SPI (HSPI) - optional
#define RFID_SS_GPIO GPIO_NUM_5
#define RFID_RST_GPIO GPIO_NUM_4
#define RFID_MOSI_GPIO GPIO_NUM_6
#define RFID_MISO_GPIO GPIO_NUM_7
#define RFID_SCK_GPIO GPIO_NUM_15

// Wi-Fi / MQTT
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""
#define MQTT_BROKER_URI "mqtt://broker.hivemq.com"
#define MQTT_CLIENT_ID "cocis-entrance"

// Access schedule defaults
#define ACCESS_START_HOUR_DEFAULT 7
#define ACCESS_END_HOUR_DEFAULT 21

// Max people
#define MAX_PEOPLE 50

// Admin auth
#define ADMIN_PIN "1234"

#endif
