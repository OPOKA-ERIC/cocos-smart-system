/* =============================================================
   CoCIS Smart Control and Monitoring System
   College of Computing and Information Sciences, Makerere University

   app_main() is the only function here.
   It sets up hardware, initialises each module, and starts the
   four background tasks. All logic lives in the other source files.
   ============================================================= */

#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "config.h"
#include "globals.h"
#include "auth.h"
#include "sd_log.h"
#include "rfid.h"
#include "rooms.h"
#include "menu.h"

static const char *TAG = "COCIS";

/* GPIO setup: outputs (LED, buzzer, room lights) and inputs (PIR). */
static void gpio_setup(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << DOOR_LOCK_GPIO)  |
                        (1ULL << BUZZER_GPIO)      |
                        (1ULL << ROOM1_LIGHT_GPIO) |
                        (1ULL << ROOM2_LIGHT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_set_level(DOOR_LOCK_GPIO,   0);
    gpio_set_level(BUZZER_GPIO,      0);
    gpio_set_level(ROOM1_LIGHT_GPIO, 0);
    gpio_set_level(ROOM2_LIGHT_GPIO, 0);

    gpio_config_t in = {
        .pin_bit_mask = (1ULL << ROOM1_PIR_GPIO) |
                        (1ULL << ROOM2_PIR_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));
}

void app_main(void)
{
    ESP_LOGI(TAG, "CoCIS starting");

    /* Shared mutexes */
    g_state_lock = xSemaphoreCreateMutex();
    g_sd_lock    = xSemaphoreCreateMutex();
    g_room_lock  = xSemaphoreCreateMutex();

    /* Hardware and storage setup */
    nvs_setup();
    config_load();
    gpio_setup();
    sd_setup();
    rfid_bus_setup();

    /* Initialise both RFID readers */
    g_entry_ok = rc522_init(&g_entry_reader, SPI2_HOST,
                            RFID_ENTRY_CS_GPIO, RFID_ENTRY_RST_GPIO) == ESP_OK;
    g_exit_ok  = rc522_init(&g_exit_reader,  SPI2_HOST,
                            RFID_EXIT_CS_GPIO,  RFID_EXIT_RST_GPIO)  == ESP_OK;

    if (!g_entry_ok) ESP_LOGE(TAG, "entry reader init failed");
    if (!g_exit_ok)  ESP_LOGE(TAG, "exit reader init failed");

    /* Timezone and starting clock */
    setenv("TZ", "EAT-3", 1);
    tzset();
    struct timeval initial_time = {.tv_sec = 1761834600, .tv_usec = 0};
    settimeofday(&initial_time, NULL);

    /* Start background tasks */
    if (g_entry_ok) xTaskCreate(rfid_task, "rfid_entry", 4096, &g_entry_reader, 5, NULL);
    if (g_exit_ok)  xTaskCreate(rfid_task, "rfid_exit",  4096, &g_exit_reader,  5, NULL);
    xTaskCreate(room_task, "rooms", 4096, NULL, 5, NULL);
    xTaskCreate(menu_task, "menu",  8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready");
}
