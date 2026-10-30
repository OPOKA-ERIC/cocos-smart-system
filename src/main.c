/* =============================================================
   CoCIS Smart Control and Monitoring System
   College of Computing and Information Sciences, Makerere University

   This one ESP32-S3 controls:
     1. The main entrance  - two RFID readers (entry + exit), a door
                             LED and a buzzer
     2. Two lecture rooms  - PIR motion sensors and automatic lights
     3. An SD card         - stores a log of every access event
     4. A serial menu      - the admin can check status and change
                             settings from the serial monitor

   HOW TO READ THIS FILE
   The file is split into numbered sections. Each section starts
   with a comment banner like the one above. Read the banners first
   to get the overall idea, then read the code below each banner.
   ============================================================= */

/* -------------------------------------------------------------
   SECTION 1: LIBRARIES WE NEED
   These #include lines pull in code that other people already wrote
   so that we do not have to write it ourselves.
   ------------------------------------------------------------- */

/* Standard C libraries */
#include <stdio.h>      /* printf, fopen, fclose */
#include <stdlib.h>     /* atoi */
#include <string.h>     /* strcmp, strcpy, strlen */
#include <time.h>       /* time, localtime, strftime */
#include <sys/time.h>   /* struct timeval, settimeofday */

/* FreeRTOS: lets us run several tasks at the same time.
   A "task" is a small program that runs forever in a loop.
   A "mutex" (semaphore) is like a key to a room: only the task
   holding the key may change the shared variables. */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ESP-IDF drivers and services */
#include "driver/gpio.h"        /* control GPIO pins */
#include "driver/spi_master.h"  /* SPI bus for the RFID readers and SD card */
#include "esp_log.h"            /* ESP_LOGI / ESP_LOGW logging */
#include "esp_system.h"         /* esp_restart */
#include "esp_vfs_fat.h"        /* mount the SD card as a FAT filesystem */
#include "nvs_flash.h"          /* non-volatile storage that survives reboot */
#include "nvs.h"                /* key/value storage API */
#include "sdmmc_cmd.h"          /* SD card commands */

/* Our own headers */
#include "config.h"   /* pin numbers and default settings */
#include "rc522.h"    /* the RFID reader driver */

/* A short tag that is printed in front of every log message. */
static const char *TAG = "COCIS";

/* -------------------------------------------------------------
   SECTION 2: OUR OWN DATA TYPES
   A "struct" groups several variables together under one name.
   ------------------------------------------------------------- */

/* One allowed card: its UID and the owner's name. */
typedef struct {
    char uid[24];   /* card ID as text, e.g. "01:02:03:04" */
    char name[24];  /* the person who owns the card */
} allowed_user_t;

/* One person who is currently inside the building. */
typedef struct {
    char   uid[24];   /* their card UID */
    char   name[24];  /* their name */
    time_t entered;   /* the time they entered */
} occupant_t;

/* -------------------------------------------------------------
   SECTION 3: THE RFID ALLOW-LIST
   Only cards listed here may enter.
   Cards that are not in this list are denied.
   ------------------------------------------------------------- */
static const allowed_user_t s_allowed[] = {
    {"01:02:03:04",          "Blue Card (Alice)"},
    {"55:66:77:88",          "Yellow Card (Bob)"},
    {"AA:BB:CC:DD",          "Red Card (Carol)"},
    {"04:11:22:33:44:55:66", "NFC Tag (Dave)"},
    {"04:11:22:33",          "NFC Tag (Dave)"},
};

/* -------------------------------------------------------------
   SECTION 4: GLOBAL STATE - ENTRANCE
   These variables are used by more than one function, so they are
   declared here at the top of the file.
   ------------------------------------------------------------- */

static rc522_t s_entry_reader;       /* the RFID reader at the entry */
static rc522_t s_exit_reader;        /* the RFID reader at the exit */
static bool    s_entry_ok = false;   /* true if the entry reader started */
static bool    s_exit_ok  = false;   /* true if the exit reader started */

/* Mutexes protect shared data. Always take the mutex before changing
   the data, and give it back as soon as you are done. */
static SemaphoreHandle_t s_state_lock = NULL; /* protects the occupant list */
static SemaphoreHandle_t s_sd_lock    = NULL; /* protects the SD log file */

static nvs_handle_t s_nvs;            /* handle used to read/write saved settings */
static bool         s_nvs_ok = false; /* true if NVS opened correctly */

static sdmmc_card_t *s_card  = NULL;  /* SD card handle */
static bool          s_sd_ok = false; /* true if the SD card is mounted */

/* The access schedule is stored as "minutes since midnight".
   For example 07:00 is stored as 420 (7 * 60). */
static int s_sched_start_min = ACCESS_START_HOUR_DEFAULT * 60;
static int s_sched_end_min   = ACCESS_END_HOUR_DEFAULT   * 60;

/* Admin login state */
static char       s_admin_pin[12] = ADMIN_PIN_DEFAULT;
static bool       s_authenticated = false;
static TickType_t s_auth_until    = 0;   /* tick at which login expires */

/* Everyone currently inside the building */
static occupant_t s_occupants[MAX_PEOPLE];
static int        s_occupant_count = 0;

/* -------------------------------------------------------------
   SECTION 5: GLOBAL STATE - ROOMS
   ------------------------------------------------------------- */
static SemaphoreHandle_t s_room_lock = NULL; /* protects the room variables */

static bool       s_room1_occupied     = false;
static bool       s_room1_light        = false;
static char       s_room1_mode[8]      = "AUTO"; /* AUTO / ON / OFF */
static TickType_t s_room1_last_motion  = 0;

static bool       s_room2_occupied     = false;
static bool       s_room2_light        = false;
static char       s_room2_mode[8]      = "AUTO";
static TickType_t s_room2_last_motion  = 0;

/* =============================================================
   SECTION 6: SMALL TIME HELPERS
   ============================================================= */

/* Put the current date and time into "out" as readable text. */
static void fmt_time(char *out, size_t len)
{
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    /* If the clock was never set, the year will be around 1970. */
    if (t.tm_year < 100) {
        strlcpy(out, "1970-01-01 00:00:00", len);
        return;
    }

    strftime(out, len, "%Y-%m-%d %H:%M:%S", &t);
}

/* Turn "minutes since midnight" into an "HH:MM" string. */
static void fmt_hhmm(int mins, char *out, size_t len)
{
    snprintf(out, len, "%02d:%02d", mins / 60, mins % 60);
}

/* =============================================================
   SECTION 7: ACCESS SCHEDULE CHECK
   Returns true when the current time is inside the allowed window.
   If the clock is not set yet, we allow access (fail open) so the
   system is still usable during setup.
   ============================================================= */
static bool access_window_open(void)
{
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    /* Clock not set: allow access. */
    if (t.tm_year < 100) {
        return true;
    }

    int current_min = t.tm_hour * 60 + t.tm_min;

    /* Normal case, e.g. 07:00 - 21:00. */
    if (s_sched_start_min <= s_sched_end_min) {
        return (current_min >= s_sched_start_min &&
                current_min <= s_sched_end_min);
    }

    /* Overnight case, e.g. 22:00 - 06:00. */
    return (current_min >= s_sched_start_min ||
            current_min <= s_sched_end_min);
}

/* =============================================================
   SECTION 8: NON-VOLATILE STORAGE (NVS)
   NVS is flash memory that keeps its data after a reboot.
   We save the schedule and the admin PIN there.
   ============================================================= */

/* Open the NVS storage area. Call this once at startup. */
static void nvs_setup(void)
{
    esp_err_t err = nvs_flash_init();

    /* If the storage is full or from an old version, erase and retry. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err == ESP_OK) {
        s_nvs_ok = true;
    } else {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
    }
}

/* Load the saved schedule and PIN. If nothing was saved, keep defaults. */
static void config_load(void)
{
    /* Start with the default values from config.h. */
    s_sched_start_min = ACCESS_START_HOUR_DEFAULT * 60;
    s_sched_end_min   = ACCESS_END_HOUR_DEFAULT   * 60;
    strlcpy(s_admin_pin, ADMIN_PIN_DEFAULT, sizeof(s_admin_pin));

    if (!s_nvs_ok) {
        return;
    }

    /* Replace each default with the saved value, if one exists. */
    uint16_t value = 0;
    if (nvs_get_u16(s_nvs, "start", &value) == ESP_OK) {
        s_sched_start_min = value;
    }
    if (nvs_get_u16(s_nvs, "end", &value) == ESP_OK) {
        s_sched_end_min = value;
    }

    size_t len = sizeof(s_admin_pin);
    if (nvs_get_str(s_nvs, "pin", s_admin_pin, &len) != ESP_OK) {
        strlcpy(s_admin_pin, ADMIN_PIN_DEFAULT, sizeof(s_admin_pin));
    }
}

/* Save the current schedule and PIN into NVS. */
static void config_save(void)
{
    if (!s_nvs_ok) {
        return;
    }

    nvs_set_u16(s_nvs, "start", (uint16_t)s_sched_start_min);
    nvs_set_u16(s_nvs, "end",   (uint16_t)s_sched_end_min);
    nvs_set_str(s_nvs, "pin",   s_admin_pin);
    nvs_commit(s_nvs);
}

/* =============================================================
   SECTION 9: GPIO SETUP
   GPIO pins are the physical legs on the ESP32. Some are outputs
   (LED, buzzer) and some are inputs (PIR sensors).
   ============================================================= */
static void gpio_setup(void)
{
    /* Configure all output pins in one go. */
    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << DOOR_LOCK_GPIO)  |
                        (1ULL << BUZZER_GPIO)      |
                        (1ULL << ROOM1_LIGHT_GPIO) |
                        (1ULL << ROOM2_LIGHT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));

    /* Everything starts switched off. */
    gpio_set_level(DOOR_LOCK_GPIO,   0);
    gpio_set_level(BUZZER_GPIO,      0);
    gpio_set_level(ROOM1_LIGHT_GPIO, 0);
    gpio_set_level(ROOM2_LIGHT_GPIO, 0);

    /* Configure the PIR sensor pins as inputs. */
    gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << ROOM1_PIR_GPIO) |
                        (1ULL << ROOM2_PIR_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_config));
}

/* =============================================================
   SECTION 10: SD CARD SETUP
   Mount the SD card so we can write a log file on it.
   ============================================================= */
static void sd_setup(void)
{
    /* First set up the SPI bus that the SD card uses. */
    spi_bus_config_t bus_config = {
        .mosi_io_num     = SD_MOSI_GPIO,
        .miso_io_num     = SD_MISO_GPIO,
        .sclk_io_num     = SD_CLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Describe the SD card device that sits on that bus. */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = SPI3_HOST;
    host.max_freq_khz = 4000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id               = SPI3_HOST;
    slot.gpio_cs               = SD_CS_GPIO;

    /* Mount options. If mounting fails, try to format the card. */
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot,
                                  &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return;
    }

    s_sd_ok = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
}

/* =============================================================
   SECTION 11: RFID SPI BUS SETUP
   Both RFID readers share one SPI bus (SPI2). Each reader has its
   own chip-select pin, so we can talk to one at a time.
   ============================================================= */
static void rfid_bus_setup(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num     = RFID_MOSI_GPIO,
        .miso_io_num     = RFID_MISO_GPIO,
        .sclk_io_num     = RFID_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO));
}

/* =============================================================
   SECTION 12: SD CARD LOGGING
   Every entry and exit is written to a CSV file on the SD card.
   ============================================================= */

/* Write one line to the log file. */
static void sd_log_line(const char *line)
{
    if (!s_sd_ok) {
        return;
    }

    /* Only one task may write at a time. */
    if (s_sd_lock == NULL ||
        xSemaphoreTake(s_sd_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }

    FILE *file = fopen(SD_LOG_FILE, "a");
    if (file != NULL) {
        /* If the file is empty, write the header row first. */
        fseek(file, 0, SEEK_END);
        if (ftell(file) == 0) {
            fprintf(file, "timestamp,event,uid,name,inside,result\n");
        }
        fprintf(file, "%s\n", line);
        fclose(file);
    }

    xSemaphoreGive(s_sd_lock);
}

/* Build a log line, then print it and save it to the SD card. */
static void log_access(const char *event, const char *uid,
                       const char *name, int inside, const char *result)
{
    char timestamp[32];
    fmt_time(timestamp, sizeof(timestamp));

    char line[192];
    snprintf(line, sizeof(line), "%s,%s,%s,%s,%d,%s",
             timestamp, event, uid, name, inside, result);

    ESP_LOGI(TAG, "%s", line);
    sd_log_line(line);
}

/* =============================================================
   SECTION 13: THE OCCUPANT LIST
   This is the list of people who are inside the building right now.
   ============================================================= */

/* Look for a UID in the allow-list. Returns a pointer or NULL. */
static const allowed_user_t *find_user(const char *uid)
{
    int total = sizeof(s_allowed) / sizeof(s_allowed[0]);

    for (int i = 0; i < total; i++) {
        if (strcmp(s_allowed[i].uid, uid) == 0) {
            return &s_allowed[i];
        }
    }

    return NULL;
}

/* Look for a UID in the occupant list. Returns the index or -1. */
static int find_occupant(const char *uid)
{
    for (int i = 0; i < s_occupant_count; i++) {
        if (strcmp(s_occupants[i].uid, uid) == 0) {
            return i;
        }
    }

    return -1;
}

/* Add one person to the occupant list. Returns false if it is full. */
static bool add_occupant(const char *uid, const char *name)
{
    if (s_occupant_count >= MAX_PEOPLE) {
        return false;
    }

    int index = s_occupant_count;
    strlcpy(s_occupants[index].uid,  uid,  sizeof(s_occupants[0].uid));
    strlcpy(s_occupants[index].name, name, sizeof(s_occupants[0].name));
    s_occupants[index].entered = time(NULL);
    s_occupant_count++;

    return true;
}

/* Remove the person at "index" and move everyone after them left. */
static void remove_occupant(int index)
{
    for (int i = index; i < s_occupant_count - 1; i++) {
        s_occupants[i] = s_occupants[i + 1];
    }
    s_occupant_count--;
}

/* =============================================================
   SECTION 14: BUZZER AND DOOR LED FEEDBACK
   One beep means access granted. Three beeps mean denied.
   ============================================================= */

/* Beep the buzzer a number of times. Each beep lasts a few ms. */
static void buzzer_beep(int times, int milliseconds)
{
    for (int i = 0; i < times; i++) {
        int cycles = milliseconds / 2;
        if (cycles < 1) {
            cycles = 1;
        }

        /* Toggle the buzzer on and off to make a sound. */
        for (int c = 0; c < cycles; c++) {
            gpio_set_level(BUZZER_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1));
            gpio_set_level(BUZZER_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        /* Small gap between beeps. */
        if (i + 1 < times) {
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }
}

/* Access granted: light the door LED and give one short beep. */
static void feedback_grant(void)
{
    gpio_set_level(DOOR_LOCK_GPIO, 1);
    buzzer_beep(1, 120);
    vTaskDelay(pdMS_TO_TICKS(400));
    gpio_set_level(DOOR_LOCK_GPIO, 0);
}

/* Access denied: three short beeps. */
static void feedback_deny(void)
{
    buzzer_beep(3, 80);
}

/* =============================================================
   SECTION 15: ENTRY AND EXIT LOGIC
   These functions run when a card is scanned.
   ============================================================= */

/* A card was scanned at the entry reader. */
static void handle_entry(const char *uid)
{
    /* Look up the card owner. Unknown cards are still logged. */
    const allowed_user_t *user = find_user(uid);
    const char *name = "Unknown";
    if (user != NULL) {
        name = user->name;
    }

    bool        granted = false;
    const char *result  = "DENIED_UNKNOWN";

    /* Lock the occupant list while we check and update it. */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);

    if (user == NULL) {
        result = "DENIED_UNKNOWN";          /* card not on the allow-list */
    } else if (!access_window_open()) {
        result = "DENIED_SCHEDULE";         /* outside allowed hours */
    } else if (find_occupant(uid) >= 0) {
        result = "DENIED_DUPLICATE";        /* person is already inside */
    } else if (s_occupant_count >= MAX_PEOPLE) {
        result = "DENIED_CAPACITY";         /* building is full */
    } else {
        add_occupant(uid, user->name);
        granted = true;
        result  = "GRANTED";
    }

    int inside = s_occupant_count;

    xSemaphoreGive(s_state_lock);

    log_access("ENTRY", uid, name, inside, result);

    if (granted) {
        feedback_grant();
    } else {
        feedback_deny();
    }
}

/* A card was scanned at the exit reader. */
static void handle_exit(const char *uid)
{
    const allowed_user_t *user = find_user(uid);
    const char *name = "Unknown";
    if (user != NULL) {
        name = user->name;
    }

    bool        granted = false;
    const char *result  = "DENIED_NOT_INSIDE";

    /* Lock the occupant list while we check and update it. */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);

    int index = find_occupant(uid);
    if (index >= 0) {
        remove_occupant(index);
        granted = true;
        result  = "GRANTED";
    }

    int inside = s_occupant_count;

    xSemaphoreGive(s_state_lock);

    log_access("EXIT", uid, name, inside, result);

    if (granted) {
        feedback_grant();
    } else {
        feedback_deny();
    }
}

/* =============================================================
   SECTION 16: THE RFID TASK
   Runs forever for one reader. It checks for a card every 150 ms.
   The same function is used for both readers; the reader is passed
   in as "arg", so we know whether this is the entry or exit task.
   ============================================================= */

/* Turn the raw UID bytes into text like "01:02:03:04". */
static void uid_to_text(const uint8_t *raw, uint8_t len,
                        char *out, size_t out_size)
{
    out[0] = '\0';

    for (int i = 0; i < len && i < 10; i++) {
        char byte_text[6];

        if (i < len - 1) {
            snprintf(byte_text, sizeof(byte_text), "%02X:", raw[i]);
        } else {
            snprintf(byte_text, sizeof(byte_text), "%02X", raw[i]);
        }

        strlcat(out, byte_text, out_size);
    }
}

static void rfid_task(void *arg)
{
    rc522_t *reader = (rc522_t *)arg;
    bool     is_entry = (reader == &s_entry_reader);

    char last_uid[24] = "";
    bool card_present = false;
    int  absent_reads = 0;

    while (true) {
        uint8_t uid[10];
        uint8_t uid_len = 0;

        if (rc522_read_uid(reader, uid, &uid_len)) {
            /* We found a card. Convert its UID to readable text. */
            char uid_text[48];
            uid_to_text(uid, uid_len, uid_text, sizeof(uid_text));

            absent_reads = 0;

            /* Only act once per tap. Ignore repeats while the card
               is still sitting on the reader. */
            if (!card_present || strcmp(uid_text, last_uid) != 0) {
                card_present = true;
                strlcpy(last_uid, uid_text, sizeof(last_uid));

                ESP_LOGI(TAG, "%s card %s",
                         is_entry ? "ENTRY" : "EXIT", uid_text);

                if (is_entry) {
                    handle_entry(uid_text);
                } else {
                    handle_exit(uid_text);
                }
            }
        } else {
            /* No card right now. After 8 misses in a row we decide
               the card has been removed. */
            if (card_present && ++absent_reads >= 8) {
                card_present = false;
                last_uid[0]  = '\0';
            }
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* =============================================================
   SECTION 17: ADMIN LOGIN
   The admin types "login 1234" in the serial monitor. The login
   stays valid for 60 seconds, then it locks itself again.
   ============================================================= */

/* Returns true if the admin is currently logged in. */
static bool is_authenticated(void)
{
    if (!s_authenticated) {
        return false;
    }

    /* Check whether the 60-second window has expired. */
    if ((int32_t)(s_auth_until - xTaskGetTickCount()) > 0) {
        return true;
    }

    s_authenticated = false;
    return false;
}

/* Mark the admin as logged in and set the expiry time. */
static void auth_ok(void)
{
    s_authenticated = true;
    s_auth_until = xTaskGetTickCount() + pdMS_TO_TICKS(AUTH_WINDOW_MS);
}

/* A valid PIN is 4 to 7 digits long. */
static bool valid_pin(const char *pin)
{
    size_t len = strlen(pin);

    if (len < 4 || len > 7) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }

    return true;
}

/* =============================================================
   SECTION 18: ROOM LIGHTING CONTROL
   ============================================================= */

/* Decide whether a light should be on. In AUTO mode the light
   follows the PIR sensor. In ON/OFF mode we obey the user. */
static bool light_should_be_on(const char *mode, bool occupied)
{
    if (strcmp(mode, "ON") == 0) {
        return true;
    }
    if (strcmp(mode, "OFF") == 0) {
        return false;
    }
    return occupied; /* AUTO mode */
}

/* Manually set a room's mode to "ON", "OFF" or "AUTO". */
static void room_set_mode(int room, const char *mode)
{
    /* Ignore anything that is not a valid mode. */
    if (strcmp(mode, "ON")   != 0 &&
        strcmp(mode, "OFF")  != 0 &&
        strcmp(mode, "AUTO") != 0) {
        return;
    }

    if (room == 1) {
        xSemaphoreTake(s_room_lock, portMAX_DELAY);
        strlcpy(s_room1_mode, mode, sizeof(s_room1_mode));
        s_room1_light = light_should_be_on(mode, s_room1_occupied);
        xSemaphoreGive(s_room_lock);

        gpio_set_level(ROOM1_LIGHT_GPIO, s_room1_light ? 1 : 0);
        ESP_LOGI(TAG, "ROOM1 mode=%s light=%d", mode, s_room1_light);

    } else if (room == 2) {
        xSemaphoreTake(s_room_lock, portMAX_DELAY);
        strlcpy(s_room2_mode, mode, sizeof(s_room2_mode));
        s_room2_light = light_should_be_on(mode, s_room2_occupied);
        xSemaphoreGive(s_room_lock);

        gpio_set_level(ROOM2_LIGHT_GPIO, s_room2_light ? 1 : 0);
        ESP_LOGI(TAG, "ROOM2 mode=%s light=%d", mode, s_room2_light);
    }
}

/* =============================================================
   SECTION 19: THE ROOM TASK
   Runs every 200 ms. It reads both PIR sensors and updates the
   lights. If no motion is seen for OCCUPANCY_TIMEOUT_MS, the room
   is treated as empty and the light turns off (in AUTO mode).
   ============================================================= */
static void room_task(void *arg)
{
    (void)arg; /* we do not use the task argument here */

    s_room1_last_motion = xTaskGetTickCount();
    s_room2_last_motion = xTaskGetTickCount();

    while (true) {
        TickType_t now = xTaskGetTickCount();

        /* 1 means motion detected, 0 means no motion. */
        int room1_motion = gpio_get_level(ROOM1_PIR_GPIO);
        int room2_motion = gpio_get_level(ROOM2_PIR_GPIO);

        xSemaphoreTake(s_room_lock, portMAX_DELAY);
        bool changed = false;

        /* --- Room 1 --- */
        if (room1_motion) {
            s_room1_last_motion = now;
            if (!s_room1_occupied) {
                s_room1_occupied = true;
                changed = true;
            }
        } else if (s_room1_occupied &&
                   (now - s_room1_last_motion) > pdMS_TO_TICKS(OCCUPANCY_TIMEOUT_MS)) {
            s_room1_occupied = false;
            changed = true;
        }

        bool room1_light_before = s_room1_light;
        s_room1_light = light_should_be_on(s_room1_mode, s_room1_occupied);
        if (room1_light_before != s_room1_light) {
            changed = true;
        }

        /* --- Room 2 --- */
        if (room2_motion) {
            s_room2_last_motion = now;
            if (!s_room2_occupied) {
                s_room2_occupied = true;
                changed = true;
            }
        } else if (s_room2_occupied &&
                   (now - s_room2_last_motion) > pdMS_TO_TICKS(OCCUPANCY_TIMEOUT_MS)) {
            s_room2_occupied = false;
            changed = true;
        }

        bool room2_light_before = s_room2_light;
        s_room2_light = light_should_be_on(s_room2_mode, s_room2_occupied);
        if (room2_light_before != s_room2_light) {
            changed = true;
        }

        xSemaphoreGive(s_room_lock);

        /* Apply the new light states to the real pins. */
        gpio_set_level(ROOM1_LIGHT_GPIO, s_room1_light ? 1 : 0);
        gpio_set_level(ROOM2_LIGHT_GPIO, s_room2_light ? 1 : 0);

        if (changed) {
            ESP_LOGI(TAG,
                "ROOM1: occ=%d light=%d mode=%s | ROOM2: occ=%d light=%d mode=%s",
                s_room1_occupied, s_room1_light, s_room1_mode,
                s_room2_occupied, s_room2_light, s_room2_mode);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* =============================================================
   SECTION 20: SERIAL MENU - DISPLAY FUNCTIONS
   These functions only print information to the serial monitor.
   ============================================================= */

/* Print the list of commands. */
static void print_help(void)
{
    printf("\nCoCIS Single-ESP32 commands:\n");
    printf("  status                      show hub, rooms and state\n");
    printf("  list                        list people currently inside\n");
    printf("  users                       show the RFID allow-list\n");
    printf("  logs [n]                    show the last n SD log lines\n");
    printf("  login <pin>                 unlock admin commands for 60 s\n");
    printf("  logout                      lock admin commands\n");
    printf("  schedule                    show the access schedule\n");
    printf("  schedule <HH:MM> <HH:MM>    change the schedule (admin)\n");
    printf("  settime YYYY-MM-DD HH:MM:SS set the clock (admin)\n");
    printf("  setpin <old> <new>          change the admin PIN (admin)\n");
    printf("  room1 on|off|auto|status    control Room 1 lighting (admin)\n");
    printf("  room2 on|off|auto|status    control Room 2 lighting (admin)\n");
    printf("  reboot                      restart the controller (admin)\n");
    printf("  help                        show this help\n\n");
    fflush(stdout);
}

/* Print the whole system status. */
static void print_status(void)
{
    char timestamp[32];
    char start[8];
    char end[8];

    fmt_time(timestamp, sizeof(timestamp));
    fmt_hhmm(s_sched_start_min, start, sizeof(start));
    fmt_hhmm(s_sched_end_min,   end,   sizeof(end));

    printf("\n=== CoCIS Single ESP32 ===\n");
    printf("Time       : %s\n", timestamp);
    printf("Schedule   : %s-%s (%s)\n", start, end,
           access_window_open() ? "OPEN" : "CLOSED");
    printf("SD card    : %s\n", s_sd_ok ? "mounted" : "not mounted");
    printf("Admin      : %s\n", is_authenticated() ? "unlocked" : "locked");

    /* Show everyone who is inside. Lock the list while reading it. */
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    printf("Inside     : %d/%d\n", s_occupant_count, MAX_PEOPLE);

    for (int i = 0; i < s_occupant_count; i++) {
        char when[32];
        struct tm time_in;
        localtime_r(&s_occupants[i].entered, &time_in);
        strftime(when, sizeof(when), "%H:%M:%S", &time_in);

        printf("  %2d. %-22s %s since %s\n",
               i + 1, s_occupants[i].name, s_occupants[i].uid, when);
    }
    xSemaphoreGive(s_state_lock);

    /* Show the room states. */
    xSemaphoreTake(s_room_lock, portMAX_DELAY);
    printf("Room 1     : occupied=%d light=%d mode=%s\n",
           s_room1_occupied, s_room1_light, s_room1_mode);
    printf("Room 2     : occupied=%d light=%d mode=%s\n",
           s_room2_occupied, s_room2_light, s_room2_mode);
    xSemaphoreGive(s_room_lock);

    printf("==========================\n\n");
    fflush(stdout);
}

/* Print the RFID allow-list. */
static void print_users(void)
{
    printf("\nRFID allow-list:\n");

    int total = sizeof(s_allowed) / sizeof(s_allowed[0]);
    for (int i = 0; i < total; i++) {
        printf("  %-24s %s\n", s_allowed[i].uid, s_allowed[i].name);
    }

    printf("  All other cards are denied.\n\n");
    fflush(stdout);
}

/* Print the last n lines from the SD card log file. */
static void tail_logs(int n)
{
    if (!s_sd_ok) {
        printf("SD card is not mounted.\n");
        fflush(stdout);
        return;
    }

    /* Keep the number of lines sensible. */
    if (n <= 0) {
        n = 10;
    }
    if (n > 50) {
        n = 50;
    }

    xSemaphoreTake(s_sd_lock, portMAX_DELAY);

    FILE *file = fopen(SD_LOG_FILE, "r");
    if (file == NULL) {
        xSemaphoreGive(s_sd_lock);
        printf("No log file yet.\n");
        fflush(stdout);
        return;
    }

    /* Read only the last 4 KB, which is enough for 50 lines. */
    fseek(file, 0, SEEK_END);
    long size  = ftell(file);
    long start = (size > 4096) ? (size - 4096) : 0;
    fseek(file, start, SEEK_SET);

    char   buffer[4097];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[bytes_read] = '\0';
    fclose(file);

    xSemaphoreGive(s_sd_lock);

    /* If we started in the middle of a line, skip that broken line. */
    char *text = buffer;
    if (start > 0) {
        char *newline = strchr(buffer, '\n');
        if (newline != NULL) {
            text = newline + 1;
        }
    }

    /* Split the text into lines, then print the last n of them. */
    char *lines[64];
    int   count = 0;
    char *save  = NULL;
    char *token = strtok_r(text, "\n", &save);

    while (token != NULL && count < 64) {
        lines[count++] = token;
        token = strtok_r(NULL, "\n", &save);
    }

    int from = (count > n) ? (count - n) : 0;
    printf("\nLast %d log line(s):\n", count - from);

    for (int i = from; i < count; i++) {
        printf("%s\n", lines[i]);
    }

    printf("\n");
    fflush(stdout);
}

/* =============================================================
   SECTION 21: SERIAL MENU - COMMAND HANDLER
   This is called once for every line the admin types.
   ============================================================= */

/* Convert lowercase letters in a string to uppercase. */
static void to_uppercase(char *text)
{
    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] >= 'a' && text[i] <= 'z') {
            text[i] = text[i] - 'a' + 'A';
        }
    }
}

static void handle_menu_line(char *line)
{
    /* Trim spaces from the start of the line. */
    while (*line == ' ' || *line == '\t') {
        line++;
    }

    /* Trim spaces and newline characters from the end. */
    size_t len = strlen(line);
    while (len > 0 &&
           (line[len - 1] == '\r' || line[len - 1] == '\n' ||
            line[len - 1] == ' '  || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    if (len == 0) {
        return;
    }

    /* Split the line into a command and its arguments.
       Example: "login 1234" -> command "login", args "1234". */
    char *command = line;
    char *args    = strchr(line, ' ');

    if (args != NULL) {
        *args++ = '\0';
        while (*args == ' ') {
            args++;
        }
    } else {
        args = line + strlen(line); /* points at the end, empty string */
    }

    /* ---- help ---- */
    if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0) {
        print_help();
    }

    /* ---- status / list ---- */
    else if (strcmp(command, "status") == 0 || strcmp(command, "list") == 0) {
        print_status();
    }

    /* ---- users ---- */
    else if (strcmp(command, "users") == 0) {
        print_users();
    }

    /* ---- logs ---- */
    else if (strcmp(command, "logs") == 0) {
        int count = (*args != '\0') ? atoi(args) : 10;
        tail_logs(count);
    }

    /* ---- login <pin> ---- */
    else if (strcmp(command, "login") == 0) {
        if (*args != '\0' && strcmp(args, s_admin_pin) == 0) {
            auth_ok();
            printf("Admin unlocked for 60 s.\n");
        } else {
            log_access("AUTH", "-", "admin", s_occupant_count, "DENIED_PIN");
            printf("Wrong PIN.\n");
        }
        fflush(stdout);
    }

    /* ---- logout ---- */
    else if (strcmp(command, "logout") == 0) {
        s_authenticated = false;
        printf("Admin locked.\n");
        fflush(stdout);
    }

    /* ---- schedule [HH:MM HH:MM] ---- */
    else if (strcmp(command, "schedule") == 0) {
        char start[8];
        char end[8];
        fmt_hhmm(s_sched_start_min, start, sizeof(start));
        fmt_hhmm(s_sched_end_min,   end,   sizeof(end));

        /* With no arguments, just show the current schedule. */
        if (*args == '\0') {
            printf("Access schedule: %s-%s (%s)\n", start, end,
                   access_window_open() ? "OPEN" : "CLOSED");
            fflush(stdout);
            return;
        }

        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }

        int h1, m1, h2, m2;
        if (sscanf(args, "%d:%d %d:%d", &h1, &m1, &h2, &m2) != 4 ||
            h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23 ||
            m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) {
            printf("Usage: schedule HH:MM HH:MM\n");
            fflush(stdout);
            return;
        }

        s_sched_start_min = h1 * 60 + m1;
        s_sched_end_min   = h2 * 60 + m2;
        config_save();

        printf("Schedule saved: %02d:%02d-%02d:%02d\n", h1, m1, h2, m2);
        fflush(stdout);
    }

    /* ---- settime YYYY-MM-DD HH:MM:SS ---- */
    else if (strcmp(command, "settime") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }

        int year, month, day, hour, minute, second;
        if (sscanf(args, "%d-%d-%d %d:%d:%d",
                   &year, &month, &day, &hour, &minute, &second) != 6 ||
            year < 2020 || month < 1 || month > 12 ||
            day < 1 || day > 31 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
            second < 0 || second > 59) {
            printf("Usage: settime YYYY-MM-DD HH:MM:SS\n");
            fflush(stdout);
            return;
        }

        struct tm time_to_set = {
            .tm_year = year - 1900,
            .tm_mon  = month - 1,
            .tm_mday = day,
            .tm_hour = hour,
            .tm_min  = minute,
            .tm_sec  = second,
            .tm_isdst = -1
        };

        time_t t = mktime(&time_to_set);
        if (t < 0) {
            t = time(NULL);
        }

        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);

        char timestamp[32];
        fmt_time(timestamp, sizeof(timestamp));
        printf("Clock set to %s\n", timestamp);
        fflush(stdout);
    }

    /* ---- setpin <old> <new> ---- */
    else if (strcmp(command, "setpin") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }

        char old_pin[8] = "";
        char new_pin[8] = "";

        if (sscanf(args, "%7s %7s", old_pin, new_pin) != 2) {
            printf("Usage: setpin <old> <new>\n");
            fflush(stdout);
            return;
        }

        if (strcmp(old_pin, s_admin_pin) != 0) {
            printf("Wrong current PIN.\n");
            fflush(stdout);
            return;
        }

        if (!valid_pin(new_pin)) {
            printf("New PIN must be 4-7 digits.\n");
            fflush(stdout);
            return;
        }

        strlcpy(s_admin_pin, new_pin, sizeof(s_admin_pin));
        config_save();
        auth_ok();
        printf("PIN updated.\n");
        fflush(stdout);
    }

    /* ---- room1 / room2 on|off|auto|status ---- */
    else if (strcmp(command, "room1") == 0 || strcmp(command, "room2") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }

        int room = command[4] - '0'; /* "room1" -> 1, "room2" -> 2 */

        char action[16] = "";
        if (sscanf(args, "%15s", action) != 1) {
            printf("Usage: %s on|off|auto|status\n", command);
            fflush(stdout);
            return;
        }
        to_uppercase(action);

        if (strcmp(action, "STATUS") == 0) {
            xSemaphoreTake(s_room_lock, portMAX_DELAY);
            if (room == 1) {
                printf("Room 1: occupied=%d light=%d mode=%s\n",
                       s_room1_occupied, s_room1_light, s_room1_mode);
            } else {
                printf("Room 2: occupied=%d light=%d mode=%s\n",
                       s_room2_occupied, s_room2_light, s_room2_mode);
            }
            xSemaphoreGive(s_room_lock);
        } else if (strcmp(action, "ON")   == 0 ||
                   strcmp(action, "OFF")  == 0 ||
                   strcmp(action, "AUTO") == 0) {
            room_set_mode(room, action);
            printf("Room %d set to %s\n", room, action);
        } else {
            printf("Usage: %s on|off|auto|status\n", command);
        }

        fflush(stdout);
    }

    /* ---- reboot ---- */
    else if (strcmp(command, "reboot") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }

        printf("Rebooting...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    /* ---- unknown command ---- */
    else {
        printf("Unknown command '%s'. Type help.\n", command);
        fflush(stdout);
    }
}

/* =============================================================
   SECTION 22: THE MENU TASK
   Reads one character at a time from the serial monitor, builds a
   line, and hands the line to handle_menu_line().
   ============================================================= */
static void menu_task(void *arg)
{
    (void)arg;

    char   line[160];
    size_t length = 0;
    bool   last_was_cr = false;

    print_help();
    printf("cocis> ");
    fflush(stdout);

    while (true) {
        int c = fgetc(stdin);

        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Enter key was pressed (CR, LF, or CR+LF). */
        if (c == '\r' || c == '\n') {
            /* A CR+LF pair should only count as one Enter. */
            if (c == '\n' && last_was_cr) {
                last_was_cr = false;
                continue;
            }

            last_was_cr = (c == '\r');
            printf("\n");

            if (length > 0) {
                line[length] = '\0';
                handle_menu_line(line);
                length = 0;
            }

            printf("cocis> ");
            fflush(stdout);
            continue;
        }

        last_was_cr = false;

        /* Backspace key was pressed. */
        if (c == '\b' || c == 0x7F) {
            if (length > 0) {
                length--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        /* Add normal printable characters to the line. */
        if (c >= 0x20 && c < 0x7F && length + 1 < sizeof(line)) {
            line[length++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}

/* =============================================================
   SECTION 23: APP_MAIN - THE ENTRY POINT
   This is the first function that runs after the ESP32 boots.
   It sets everything up, then starts the background tasks.
   ============================================================= */
void app_main(void)
{
    ESP_LOGI(TAG, "CoCIS starting");

    /* Create the mutexes that protect shared data. */
    s_state_lock = xSemaphoreCreateMutex();
    s_sd_lock    = xSemaphoreCreateMutex();
    s_room_lock  = xSemaphoreCreateMutex();

    /* Set up storage, pins, SD card and the RFID bus. */
    nvs_setup();
    config_load();
    gpio_setup();
    sd_setup();
    rfid_bus_setup();

    /* Start both RFID readers. */
    s_entry_ok = rc522_init(&s_entry_reader, SPI2_HOST,
                            RFID_ENTRY_CS_GPIO, RFID_ENTRY_RST_GPIO) == ESP_OK;
    s_exit_ok  = rc522_init(&s_exit_reader, SPI2_HOST,
                            RFID_EXIT_CS_GPIO,  RFID_EXIT_RST_GPIO)  == ESP_OK;

    if (!s_entry_ok) {
        ESP_LOGE(TAG, "entry reader init failed");
    }
    if (!s_exit_ok) {
        ESP_LOGE(TAG, "exit reader init failed");
    }

    /* Use East Africa Time (UTC+3). */
    setenv("TZ", "EAT-3", 1);
    tzset();

    /* Set a starting clock so time functions work before the admin
       calls "settime". */
    struct timeval initial_time = {.tv_sec = 1761834600, .tv_usec = 0};
    settimeofday(&initial_time, NULL);

    /* Start the background tasks. The number (3, 5) is the priority:
       a bigger number means the task gets more attention. */
    if (s_entry_ok) {
        xTaskCreate(rfid_task, "rfid_entry", 4096, &s_entry_reader, 5, NULL);
    }
    if (s_exit_ok) {
        xTaskCreate(rfid_task, "rfid_exit", 4096, &s_exit_reader, 5, NULL);
    }
    xTaskCreate(room_task, "rooms", 4096, NULL, 5, NULL);
    xTaskCreate(menu_task, "menu", 8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready");
}
