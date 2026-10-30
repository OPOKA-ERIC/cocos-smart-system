#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include "driver/gpio.h"

/* ---------------------------------------------------------------
   ENTRANCE HARDWARE PINS
   The door LED lights up when access is granted.
   The buzzer beeps to give audio feedback.
--------------------------------------------------------------- */
#define DOOR_LOCK_GPIO      GPIO_NUM_2   /* yellow LED - lights when door opens */
#define BUZZER_GPIO         GPIO_NUM_18  /* buzzer - beeps on grant or deny */

/* ---------------------------------------------------------------
   SD CARD PINS  (uses SPI bus 3)
   The SD card stores a log of every entry and exit event.
--------------------------------------------------------------- */
#define SD_CS_GPIO          GPIO_NUM_10  /* chip select */
#define SD_MOSI_GPIO        GPIO_NUM_11  /* data out to SD card */
#define SD_MISO_GPIO        GPIO_NUM_13  /* data in from SD card */
#define SD_CLK_GPIO         GPIO_NUM_12  /* clock */

/* ---------------------------------------------------------------
   RFID READER PINS  (both readers share SPI bus 2)
   Each reader has its own CS and RST pin so we can talk to
   them one at a time on the same bus.
--------------------------------------------------------------- */
#define RFID_MOSI_GPIO      GPIO_NUM_6   /* shared data out */
#define RFID_MISO_GPIO      GPIO_NUM_7   /* shared data in */
#define RFID_SCK_GPIO       GPIO_NUM_15  /* shared clock */
#define RFID_ENTRY_CS_GPIO  GPIO_NUM_5   /* entry reader chip select */
#define RFID_ENTRY_RST_GPIO GPIO_NUM_4   /* entry reader reset */
#define RFID_EXIT_CS_GPIO   GPIO_NUM_16  /* exit reader chip select */
#define RFID_EXIT_RST_GPIO  GPIO_NUM_17  /* exit reader reset */

/* ---------------------------------------------------------------
   ROOM 1 PINS
   The PIR sensor detects motion. The LED is the room light.
--------------------------------------------------------------- */
#define ROOM1_PIR_GPIO      GPIO_NUM_38  /* PIR motion sensor input */
#define ROOM1_LIGHT_GPIO    GPIO_NUM_39  /* red LED - room 1 light */

/* ---------------------------------------------------------------
   ROOM 2 PINS
--------------------------------------------------------------- */
#define ROOM2_PIR_GPIO      GPIO_NUM_40  /* PIR motion sensor input */
#define ROOM2_LIGHT_GPIO    GPIO_NUM_41  /* blue LED - room 2 light */

/* ---------------------------------------------------------------
   ACCESS POLICY SETTINGS
   These are the default values used when the system first starts.
   The admin can change the schedule and PIN through the serial menu.
--------------------------------------------------------------- */
#define ACCESS_START_HOUR_DEFAULT  7      /* building opens at 07:00 */
#define ACCESS_END_HOUR_DEFAULT    21     /* building closes at 21:00 */
#define MAX_PEOPLE                 50     /* maximum people allowed inside */
#define ADMIN_PIN_DEFAULT          "1234" /* default admin PIN */
#define AUTH_WINDOW_MS             60000  /* admin session lasts 60 seconds */

/* ---------------------------------------------------------------
   ROOM LIGHTING TIMEOUT
   If no motion is detected for this many milliseconds the light
   turns off automatically.
--------------------------------------------------------------- */
#define OCCUPANCY_TIMEOUT_MS  15000  /* 15 seconds */

/* ---------------------------------------------------------------
   SD CARD FILE PATHS
--------------------------------------------------------------- */
#define SD_MOUNT_POINT   "/sdcard"
#define SD_LOG_FILE      SD_MOUNT_POINT "/access.log"
#define NVS_NAMESPACE    "cocis"  /* key used to save settings in flash */

#endif
