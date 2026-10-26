#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "config.h"
#include "mqtt_bus.h"

static const char *TAG = ROOM_TAG;

#define WIFI_CONNECTED_BIT BIT0

static bool s_occupied = false;
static bool s_light = false;
static char s_mode[8] = "AUTO";
static uint32_t s_seq = 0;
static TickType_t s_last_motion = 0;
static SemaphoreHandle_t s_lock = NULL;
static EventGroupHandle_t s_wifi_events = NULL;

static void publish_state(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool occupied = s_occupied;
    bool light = s_light;
    char mode[8];
    strlcpy(mode, s_mode, sizeof(mode));
    uint32_t seq = s_seq;
    xSemaphoreGive(s_lock);

    char payload[288];
    snprintf(payload, sizeof(payload),
             "{\"term\":\"%s\",\"occupied\":%s,\"light\":%s,\"mode\":\"%s\","
             "\"seq\":%u,\"uptime_ms\":%llu}",
             ROOM_TERM, occupied ? "true" : "false", light ? "true" : "false", mode,
             (unsigned)seq, (unsigned long long)(esp_timer_get_time() / 1000));
    mqtt_bus_publish(TOPIC_ROOM_STATE, payload, (int)strlen(payload), true);
}

static void set_mode(const char *mode)
{
    if (strcmp(mode, "ON") != 0 && strcmp(mode, "OFF") != 0 && strcmp(mode, "AUTO") != 0) {
        return;
    }

    bool light;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_mode, mode, sizeof(s_mode));
    if (strcmp(mode, "ON") == 0) {
        light = true;
    } else if (strcmp(mode, "OFF") == 0) {
        light = false;
    } else {
        light = s_occupied;
    }
    s_light = light;
    s_seq++;
    xSemaphoreGive(s_lock);

    gpio_set_level(LIGHT_GPIO, light ? 1 : 0);
    ESP_LOGI(TAG, "mode=%s light=%d", mode, light);
    publish_state();
}

static void on_mqtt_message(const char *topic, const char *payload, int len)
{
    if (strcmp(topic, TOPIC_ROOM_CMD) != 0) {
        return;
    }

    char cmd[24] = "";
    const char *src = payload;
    const char *j = strstr(payload, "\"cmd\"");
    if (j != NULL) {
        src = strchr(j, ':');
        if (src != NULL) {
            src++;
        }
    }
    if (src == NULL) {
        return;
    }
    while (*src == ' ' || *src == '"') {
        src++;
    }
    int n = 0;
    while (src[n] != '\0' && src[n] != '"' && src[n] != '}' && src[n] != '\n' &&
           n < (int)sizeof(cmd) - 1) {
        cmd[n] = src[n];
        n++;
    }
    cmd[n] = '\0';
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\r')) {
        cmd[--n] = '\0';
    }
    for (int i = 0; cmd[i] != '\0'; i++) {
        if (cmd[i] >= 'a' && cmd[i] <= 'z') {
            cmd[i] = (char)(cmd[i] - 'a' + 'A');
        }
    }

    if (strcmp(cmd, "STATUS") == 0) {
        publish_state();
    } else if (strcmp(cmd, "ON") == 0 || strcmp(cmd, "OFF") == 0 || strcmp(cmd, "AUTO") == 0) {
        set_mode(cmd);
    } else {
        ESP_LOGW(TAG, "unknown command '%s'", cmd);
    }
}

static void gpio_setup(void)
{
    gpio_config_t pir = {
        .pin_bit_mask = (1ULL << PIR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pir));

    gpio_config_t led = {
        .pin_bit_mask = (1ULL << LIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(LIGHT_GPIO, 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_events != NULL) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_events != NULL) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        ESP_LOGI(TAG, "Wi-Fi got IP");
    }
}

static void wifi_setup(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool wifi_wait_connected(int timeout_ms)
{
    if (s_wifi_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void control_task(void *arg)
{
    (void)arg;
    s_last_motion = xTaskGetTickCount();

    while (true) {
        int pir = gpio_get_level(PIR_GPIO);
        TickType_t now = xTaskGetTickCount();
        bool changed = false;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (pir) {
            s_last_motion = now;
            if (!s_occupied) {
                s_occupied = true;
                changed = true;
            }
        } else if (s_occupied &&
                   (now - s_last_motion) > pdMS_TO_TICKS(OCCUPANCY_TIMEOUT_MS)) {
            s_occupied = false;
            changed = true;
        }

        bool desired;
        if (strcmp(s_mode, "ON") == 0) {
            desired = true;
        } else if (strcmp(s_mode, "OFF") == 0) {
            desired = false;
        } else {
            desired = s_occupied;
        }
        if (desired != s_light) {
            s_light = desired;
            changed = true;
        }
        if (changed) {
            s_seq++;
        }
        bool occupied = s_occupied;
        bool light = s_light;
        xSemaphoreGive(s_lock);

        gpio_set_level(LIGHT_GPIO, light ? 1 : 0);
        if (changed) {
            ESP_LOGI(TAG, "occupied=%d light=%d mode=%s", occupied, light, s_mode);
            publish_state();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        publish_state();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s controller starting", ROOM_NAME);

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "failed to create mutex");
        return;
    }

    gpio_setup();
    wifi_setup();
    bool wifi_ok = wifi_wait_connected(30000);
    ESP_LOGI(TAG, "Wi-Fi %s", wifi_ok ? "connected" : "not connected, continuing offline");

    mqtt_bus_start(MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID, MQTT_STATUS_TOPIC, "offline",
                   on_mqtt_message);
    mqtt_bus_subscribe(TOPIC_ROOM_CMD);

    xTaskCreate(control_task, "control", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 3072, NULL, 2, NULL);

    publish_state();
    ESP_LOGI(TAG, "ready: PIR=%d light=%d", (int)PIR_GPIO, (int)LIGHT_GPIO);
}
