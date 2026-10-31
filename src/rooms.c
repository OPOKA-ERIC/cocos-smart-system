#include "rooms.h"
#include "globals.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ROOMS";

/* ---------------------------------------------------------------
   LIGHT DECISION
--------------------------------------------------------------- */
static bool light_should_be_on(const char *mode, bool occupied)
{
    if (strcmp(mode, "ON")  == 0) return true;
    if (strcmp(mode, "OFF") == 0) return false;
    return occupied; /* AUTO */
}

/* ---------------------------------------------------------------
   SET ROOM MODE  (ON / OFF / AUTO)
--------------------------------------------------------------- */
void room_set_mode(int room, const char *mode)
{
    if (strcmp(mode, "ON")   != 0 &&
        strcmp(mode, "OFF")  != 0 &&
        strcmp(mode, "AUTO") != 0) {
        return;
    }

    if (room == 1) {
        xSemaphoreTake(g_room_lock, portMAX_DELAY);
        strlcpy(g_room1_mode, mode, sizeof(g_room1_mode));
        g_room1_light = light_should_be_on(mode, g_room1_occupied);
        xSemaphoreGive(g_room_lock);

        gpio_set_level(ROOM1_LIGHT_GPIO, g_room1_light ? 1 : 0);
        ESP_LOGI(TAG, "ROOM1 mode=%s light=%d", mode, g_room1_light);

    } else if (room == 2) {
        xSemaphoreTake(g_room_lock, portMAX_DELAY);
        strlcpy(g_room2_mode, mode, sizeof(g_room2_mode));
        g_room2_light = light_should_be_on(mode, g_room2_occupied);
        xSemaphoreGive(g_room_lock);

        gpio_set_level(ROOM2_LIGHT_GPIO, g_room2_light ? 1 : 0);
        ESP_LOGI(TAG, "ROOM2 mode=%s light=%d", mode, g_room2_light);
    }
}

/* ---------------------------------------------------------------
   ROOM TASK  — polls PIR sensors every 200 ms
--------------------------------------------------------------- */
void room_task(void *arg)
{
    (void)arg;

    g_room1_last_motion = xTaskGetTickCount();
    g_room2_last_motion = xTaskGetTickCount();

    while (true) {
        TickType_t now = xTaskGetTickCount();

        int room1_motion = gpio_get_level(ROOM1_PIR_GPIO);
        int room2_motion = gpio_get_level(ROOM2_PIR_GPIO);

        xSemaphoreTake(g_room_lock, portMAX_DELAY);
        bool changed = false;

        /* --- Room 1 --- */
        if (room1_motion) {
            g_room1_last_motion = now;
            if (!g_room1_occupied) { g_room1_occupied = true; changed = true; }
        } else if (g_room1_occupied &&
                   (now - g_room1_last_motion) > pdMS_TO_TICKS(OCCUPANCY_TIMEOUT_MS)) {
            g_room1_occupied = false;
            changed = true;
        }

        bool r1_before = g_room1_light;
        g_room1_light = light_should_be_on(g_room1_mode, g_room1_occupied);
        if (r1_before != g_room1_light) changed = true;

        /* --- Room 2 --- */
        if (room2_motion) {
            g_room2_last_motion = now;
            if (!g_room2_occupied) { g_room2_occupied = true; changed = true; }
        } else if (g_room2_occupied &&
                   (now - g_room2_last_motion) > pdMS_TO_TICKS(OCCUPANCY_TIMEOUT_MS)) {
            g_room2_occupied = false;
            changed = true;
        }

        bool r2_before = g_room2_light;
        g_room2_light = light_should_be_on(g_room2_mode, g_room2_occupied);
        if (r2_before != g_room2_light) changed = true;

        xSemaphoreGive(g_room_lock);

        gpio_set_level(ROOM1_LIGHT_GPIO, g_room1_light ? 1 : 0);
        gpio_set_level(ROOM2_LIGHT_GPIO, g_room2_light ? 1 : 0);

        if (changed) {
            ESP_LOGI(TAG,
                "ROOM1: occ=%d light=%d mode=%s | ROOM2: occ=%d light=%d mode=%s",
                g_room1_occupied, g_room1_light, g_room1_mode,
                g_room2_occupied, g_room2_light, g_room2_mode);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
