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
#include "config.h"

static const char *TAG = "ENTRANCE";
static uint32_t seq = 0;
static uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef enum { EVT_NONE, EVT_ACCESS_GRANTED, EVT_ACCESS_DENIED, EVT_ROOM_OCCUPIED, EVT_ROOM_FREE } event_type_t;
typedef struct {
    uint8_t term_id; // 1=entrance, 2=room1, 3=room2
    event_type_t type;
    uint32_t seq;
    uint32_t uptime_ms;
} __attribute__((packed)) cocis_evt_t;

static void blink_lock(int times, int delay_ms)
{
    for (int i = 0; i < times; i++) {
        gpio_set_level(DOOR_LOCK_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        gpio_set_level(DOOR_LOCK_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void espnow_send_event(event_type_t type)
{
    cocis_evt_t evt = {
        .term_id = 1,
        .type = type,
        .seq = ++seq,
        .uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS)
    };
    esp_err_t ret = esp_now_send(broadcast_mac, (uint8_t *)&evt, sizeof(evt));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ESPNOW sent type=%d seq=%d", type, evt.seq);
    } else {
        ESP_LOGW(TAG, "ESPNOW send failed: %s", esp_err_to_name(ret));
    }
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
    ESP_ERROR_CHECK(esp_now_register_send_cb(NULL));
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, broadcast_mac, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = ESP_IF_WIFI_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
    ESP_LOGI(TAG, "ESP-NOW initialized (broadcast)");
}

void app_main(void)
{
    ESP_LOGI(TAG, "ENTRANCE starting (Access Demo + ESPNOW)");
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DOOR_LOCK_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(DOOR_LOCK_GPIO, 0);
    
    espnow_init();

    while (1) {
        ESP_LOGI(TAG, "ACCESS GRANTED for 1001");
        blink_lock(1, 500);
        espnow_send_event(EVT_ACCESS_GRANTED);
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "ACCESS GRANTED for 1002");
        blink_lock(2, 250);
        espnow_send_event(EVT_ACCESS_GRANTED);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
