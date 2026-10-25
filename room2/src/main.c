#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "config.h"

static const char *TAG = ROOM_TAG;
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_connected = false;
static bool occupied = false;
static bool light_on = false;
static TickType_t last_motion = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        mqtt_connected = false;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = MQTT_CLIENT_ID,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void publish_status(void)
{
    if (!mqtt_connected) return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "room", ROOM_TAG);
    cJSON_AddBoolToObject(root, "occupied", occupied);
    cJSON_AddBoolToObject(root, "light", light_on);
    char *msg = cJSON_PrintUnformatted(root);
    char topic[32];
    snprintf(topic, sizeof(topic), "cocis/%s/status", ROOM_TAG);
    esp_mqtt_client_publish(mqtt_client, topic, msg, 0, 1, 0);
    ESP_LOGI(TAG, "Published %s to %s", msg, topic);
    free(msg);
    cJSON_Delete(root);
}

static void room_task(void *pvParameters)
{
    while (1) {
        if (gpio_get_level(PIR_GPIO)) {
            last_motion = xTaskGetTickCount();
            if (!occupied) {
                occupied = true;
                light_on = true;
                gpio_set_level(LIGHT_GPIO, 1);
                ESP_LOGI(TAG, "OCCUPIED - LIGHT ON");
                publish_status();
            }
        }
        if (occupied && (xTaskGetTickCount() - last_motion) > pdMS_TO_TICKS(15000)) {
            occupied = false;
            light_on = false;
            gpio_set_level(LIGHT_GPIO, 0);
            ESP_LOGI(TAG, "UNOCCUPIED - LIGHT OFF");
            publish_status();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

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
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_config(&pir_conf));
    gpio_set_level(LIGHT_GPIO, 0);

    wifi_init();
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    mqtt_app_start();
    xTaskCreate(room_task, "room_task", 4096, NULL, 5, NULL);
}
