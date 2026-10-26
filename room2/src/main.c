#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "config.h"

static const char *TAG = "ROOM2";
static bool occupied = false;
static bool light_on = false;
static TickType_t last_motion = 0;

void app_main(void)
{
    ESP_LOGI(TAG, "ROOM1 starting (PIR + LED)");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config_t pir_conf = {
        .pin_bit_mask = (1ULL << PIR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_config(&pir_conf);

    gpio_set_level(LIGHT_GPIO, 0);
    last_motion = xTaskGetTickCount();

    while (1) {
        int pir = gpio_get_level(PIR_GPIO);
        TickType_t now = xTaskGetTickCount();
        if (pir == 1) {
            if (!occupied) {
                occupied = true;
                ESP_LOGI(TAG, "OCCUPIED");
            }
            last_motion = now;
            if (!light_on) {
                gpio_set_level(LIGHT_GPIO, 1);
                light_on = true;
                ESP_LOGI(TAG, "LIGHT ON");
            }
        }
        if (occupied && (now - last_motion) > pdMS_TO_TICKS(15000)) {
            occupied = false;
            if (light_on) {
                gpio_set_level(LIGHT_GPIO, 0);
                light_on = false;
                ESP_LOGI(TAG, "LIGHT OFF");
            }
            ESP_LOGI(TAG, "UNOCCUPIED");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
