#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "config.h"

static const char *TAG = "ROOM1";
static bool occupied = false;
static bool light_on = false;
static TickType_t last_motion = 0;
static uint32_t seq_rx = 0;

typedef enum { EVT_NONE, EVT_ACCESS_GRANTED, EVT_ACCESS_DENIED, EVT_ROOM_OCCUPIED, EVT_ROOM_FREE } event_type_t;
typedef struct {
    uint8_t term_id; // 1=entrance, 2=room1, 3=room2
    event_type_t type;
    uint32_t seq;
    uint32_t uptime_ms;
} __attribute__((packed)) cocis_evt_t;

static void on_recv(const uint8_t *mac_addr, const uint8_t *data, int data_len)
{
    if (data_len != sizeof(cocis_evt_t)) {
        return;
    }
    cocis_evt_t evt;
    memcpy(&evt, data, sizeof(evt));
    if (evt.seq == seq_rx) {
        return;
    }
    seq_rx = evt.seq;
    ESP_LOGI(TAG, "RX ESPNOW from term=%d type=%d seq=%d uptime=%d", evt.term_id, evt.type, evt.seq, evt.uptime_ms);
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_LOGI(TAG, "ESP-NOW initialized (receiver)");
}

void app_main(void)
{
    ESP_LOGI(TAG, "ROOM1 starting (PIR + LED + ESPNOW)");
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
    
    espnow_init();

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
