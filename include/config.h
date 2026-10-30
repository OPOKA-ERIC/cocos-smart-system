#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

// Door feedback (Entrance)
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

// Room 1 (remapped to avoid conflicts)
#define ROOM1_PIR_GPIO      GPIO_NUM_38
#define ROOM1_LIGHT_GPIO    GPIO_NUM_39

// Room 2 (remapped to avoid conflicts)
#define ROOM2_PIR_GPIO      GPIO_NUM_40
#define ROOM2_LIGHT_GPIO    GPIO_NUM_41

// Access policy
#define ACCESS_START_HOUR_DEFAULT 7
#define ACCESS_END_HOUR_DEFAULT   21
#define MAX_PEOPLE                50
#define ADMIN_PIN_DEFAULT         "1234"
#define AUTH_WINDOW_MS            60000

// Room timing
#define OCCUPANCY_TIMEOUT_MS 15000

// Storage
#define SD_MOUNT_POINT      "/sdcard"
#define SD_LOG_FILE         SD_MOUNT_POINT "/access.log"
#define NVS_NAMESPACE       "cocis"

#endif
