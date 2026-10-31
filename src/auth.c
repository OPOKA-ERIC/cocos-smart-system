#include "auth.h"
#include "globals.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "AUTH";
static nvs_handle_t s_nvs;
static bool         s_nvs_ok = false;

/* ---------------------------------------------------------------
   TIME HELPERS
--------------------------------------------------------------- */
void fmt_time(char *out, size_t len)
{
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    if (t.tm_year < 100) {
        strlcpy(out, "1970-01-01 00:00:00", len);
        return;
    }
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &t);
}

void fmt_hhmm(int mins, char *out, size_t len)
{
    snprintf(out, len, "%02d:%02d", mins / 60, mins % 60);
}

/* ---------------------------------------------------------------
   ACCESS SCHEDULE
--------------------------------------------------------------- */
bool access_window_open(void)
{
    time_t    now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    if (t.tm_year < 100) {
        return true;  /* clock not set: fail open */
    }

    int current_min = t.tm_hour * 60 + t.tm_min;

    if (g_sched_start_min <= g_sched_end_min) {
        return (current_min >= g_sched_start_min &&
                current_min <= g_sched_end_min);
    }
    return (current_min >= g_sched_start_min ||
            current_min <= g_sched_end_min);
}

/* ---------------------------------------------------------------
   NVS SETUP AND CONFIG LOAD / SAVE
--------------------------------------------------------------- */
void nvs_setup(void)
{
    esp_err_t err = nvs_flash_init();

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

void config_load(void)
{
    g_sched_start_min = ACCESS_START_HOUR_DEFAULT * 60;
    g_sched_end_min   = ACCESS_END_HOUR_DEFAULT   * 60;
    strlcpy(g_admin_pin, ADMIN_PIN_DEFAULT, sizeof(g_admin_pin));

    if (!s_nvs_ok) return;

    uint16_t value = 0;
    if (nvs_get_u16(s_nvs, "start", &value) == ESP_OK) g_sched_start_min = value;
    if (nvs_get_u16(s_nvs, "end",   &value) == ESP_OK) g_sched_end_min   = value;

    size_t len = sizeof(g_admin_pin);
    if (nvs_get_str(s_nvs, "pin", g_admin_pin, &len) != ESP_OK) {
        strlcpy(g_admin_pin, ADMIN_PIN_DEFAULT, sizeof(g_admin_pin));
    }
}

void config_save(void)
{
    if (!s_nvs_ok) return;

    nvs_set_u16(s_nvs, "start", (uint16_t)g_sched_start_min);
    nvs_set_u16(s_nvs, "end",   (uint16_t)g_sched_end_min);
    nvs_set_str(s_nvs, "pin",   g_admin_pin);
    nvs_commit(s_nvs);
}

/* ---------------------------------------------------------------
   ADMIN LOGIN
--------------------------------------------------------------- */
bool is_authenticated(void)
{
    if (!g_authenticated) return false;

    if ((int32_t)(g_auth_until - xTaskGetTickCount()) > 0) return true;

    g_authenticated = false;
    return false;
}

void auth_ok(void)
{
    g_authenticated = true;
    g_auth_until    = xTaskGetTickCount() + pdMS_TO_TICKS(AUTH_WINDOW_MS);
}

bool valid_pin(const char *pin)
{
    size_t len = strlen(pin);
    if (len < 4 || len > 7) return false;

    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return true;
}
