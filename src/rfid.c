#include "rfid.h"
#include "globals.h"
#include "occupants.h"
#include "sd_log.h"
#include "auth.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "RFID";

/* ---------------------------------------------------------------
   RFID SPI BUS SETUP
--------------------------------------------------------------- */
void rfid_bus_setup(void)
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

/* ---------------------------------------------------------------
   BUZZER AND DOOR LED FEEDBACK
--------------------------------------------------------------- */
static void buzzer_beep(int times, int milliseconds)
{
    for (int i = 0; i < times; i++) {
        int cycles = milliseconds / 2;
        if (cycles < 1) cycles = 1;

        for (int c = 0; c < cycles; c++) {
            gpio_set_level(BUZZER_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1));
            gpio_set_level(BUZZER_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (i + 1 < times) vTaskDelay(pdMS_TO_TICKS(80));
    }
}

static void feedback_grant(void)
{
    gpio_set_level(DOOR_LOCK_GPIO, 1);
    buzzer_beep(1, 120);
    vTaskDelay(pdMS_TO_TICKS(400));
    gpio_set_level(DOOR_LOCK_GPIO, 0);
}

static void feedback_deny(void)
{
    buzzer_beep(3, 80);
}

/* ---------------------------------------------------------------
   ENTRY LOGIC
--------------------------------------------------------------- */
static void handle_entry(const char *uid)
{
    const allowed_user_t *user = find_user(uid);
    const char *name  = (user != NULL) ? user->name : "Unknown";
    bool        granted = false;
    const char *result  = "DENIED_UNKNOWN";

    xSemaphoreTake(g_state_lock, portMAX_DELAY);

    if (user == NULL) {
        result = "DENIED_UNKNOWN";
    } else if (!access_window_open()) {
        result = "DENIED_SCHEDULE";
    } else if (find_occupant(uid) >= 0) {
        result = "DENIED_DUPLICATE";
    } else if (g_occupant_count >= MAX_PEOPLE) {
        result = "DENIED_CAPACITY";
    } else {
        add_occupant(uid, user->name);
        granted = true;
        result  = "GRANTED";
    }

    int inside = g_occupant_count;
    xSemaphoreGive(g_state_lock);

    log_access("ENTRY", uid, name, inside, result);

    if (granted) feedback_grant(); else feedback_deny();
}

/* ---------------------------------------------------------------
   EXIT LOGIC
--------------------------------------------------------------- */
static void handle_exit(const char *uid)
{
    const allowed_user_t *user = find_user(uid);
    const char *name  = (user != NULL) ? user->name : "Unknown";
    bool        granted = false;
    const char *result  = "DENIED_NOT_INSIDE";

    xSemaphoreTake(g_state_lock, portMAX_DELAY);

    int index = find_occupant(uid);
    if (index >= 0) {
        remove_occupant(index);
        granted = true;
        result  = "GRANTED";
    }

    int inside = g_occupant_count;
    xSemaphoreGive(g_state_lock);

    log_access("EXIT", uid, name, inside, result);

    if (granted) feedback_grant(); else feedback_deny();
}

/* ---------------------------------------------------------------
   UID BYTES -> TEXT  e.g. "01:02:03:04"
--------------------------------------------------------------- */
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

/* ---------------------------------------------------------------
   RFID TASK  (one instance per reader)
--------------------------------------------------------------- */
void rfid_task(void *arg)
{
    rc522_t *reader   = (rc522_t *)arg;
    bool     is_entry = (reader == &g_entry_reader);

    char last_uid[24] = "";
    bool card_present = false;
    int  absent_reads = 0;

    while (true) {
        uint8_t uid[10];
        uint8_t uid_len = 0;

        if (rc522_read_uid(reader, uid, &uid_len)) {
            char uid_text[48];
            uid_to_text(uid, uid_len, uid_text, sizeof(uid_text));

            absent_reads = 0;

            if (!card_present || strcmp(uid_text, last_uid) != 0) {
                card_present = true;
                strlcpy(last_uid, uid_text, sizeof(last_uid));

                ESP_LOGI(TAG, "%s card %s",
                         is_entry ? "ENTRY" : "EXIT", uid_text);

                if (is_entry) handle_entry(uid_text);
                else          handle_exit(uid_text);
            }
        } else {
            if (card_present && ++absent_reads >= 8) {
                card_present  = false;
                last_uid[0]   = '\0';
            }
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}
