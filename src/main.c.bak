#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#include "config.h"
#include "mqtt_bus.h"
#include "rc522.h"

static const char *TAG = "ENTRANCE";

#define WIFI_CONNECTED_BIT BIT0

typedef struct {
    const char *uid;
    const char *name;
} allowed_user_t;

static const allowed_user_t s_allowed[] = {
    {"01:02:03:04", "Blue Card (Alice)"},
    {"55:66:77:88", "Yellow Card (Bob)"},
    {"AA:BB:CC:DD", "Red Card (Carol)"},
    {"04:11:22:33:44:55:66", "NFC Admin (Dave)"},
};

typedef struct {
    char uid[24];
    char name[24];
    time_t entered;
} occupant_t;

typedef struct {
    bool valid;
    bool online;
    bool occupied;
    bool light;
    char mode[8];
    uint32_t seq;
} room_state_t;

static occupant_t s_occupants[MAX_PEOPLE];
static int s_occupant_count = 0;
static SemaphoreHandle_t s_state_lock = NULL;
static SemaphoreHandle_t s_sd_lock = NULL;

static room_state_t s_room1;
static room_state_t s_room2;

static int s_sched_start_min = ACCESS_START_HOUR_DEFAULT * 60;
static int s_sched_end_min = ACCESS_END_HOUR_DEFAULT * 60;
static char s_admin_pin[8] = ADMIN_PIN_DEFAULT;
static nvs_handle_t s_nvs;
static bool s_nvs_ok = false;

static sdmmc_card_t *s_card = NULL;
static bool s_sd_ok = false;

static EventGroupHandle_t s_wifi_events = NULL;
static uint32_t s_event_seq = 0;

static bool s_authenticated = false;
static TickType_t s_auth_until = 0;

static rc522_t s_entry_reader;
static rc522_t s_exit_reader;

typedef struct {
    rc522_t *reader;
    const char *term;
    bool entry;
} reader_ctx_t;

static reader_ctx_t s_entry_ctx = {.reader = &s_entry_reader, .term = "ENTRY", .entry = true};
static reader_ctx_t s_exit_ctx = {.reader = &s_exit_reader, .term = "EXIT", .entry = false};

static void publish_state(void);

static bool time_valid(void)
{
    return time(NULL) > 1700000000;
}

static void fmt_time(char *out, size_t n)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (!time_valid()) {
        strlcpy(out, "unsynced", n);
        return;
    }
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static void fmt_hhmm(int minutes, char *out, size_t n)
{
    snprintf(out, n, "%02d:%02d", minutes / 60, minutes % 60);
}

static bool access_window_open(void)
{
    if (!time_valid()) {
        return true;
    }
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int cur = tm_now.tm_hour * 60 + tm_now.tm_min;
    if (s_sched_start_min <= s_sched_end_min) {
        return cur >= s_sched_start_min && cur < s_sched_end_min;
    }
    return cur >= s_sched_start_min || cur < s_sched_end_min;
}

static void uid_to_str(const uint8_t *uid, uint8_t len, char *out, size_t n)
{
    size_t pos = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < len; i++) {
        int wrote = snprintf(out + pos, n - pos, "%02X%s", uid[i], (i + 1 < len) ? ":" : "");
        if (wrote < 0 || (size_t)wrote >= n - pos) {
            break;
        }
        pos += (size_t)wrote;
    }
}

static bool json_bool(const char *json, const char *key, bool def)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return def;
    }
    p += strlen(pat);
    while (*p == ' ') {
        p++;
    }
    if (strncmp(p, "true", 4) == 0) {
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        return false;
    }
    return def;
}

static uint32_t json_u32(const char *json, const char *key, uint32_t def)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        return def;
    }
    p += strlen(pat);
    while (*p == ' ') {
        p++;
    }
    return (uint32_t)strtoul(p, NULL, 10);
}

static void json_str(const char *json, const char *key, char *out, size_t n)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (p == NULL) {
        out[0] = '\0';
        return;
    }
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (end == NULL) {
        out[0] = '\0';
        return;
    }
    size_t len = (size_t)(end - p);
    if (len >= n) {
        len = n - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
}

static void update_room(room_state_t *room, const char *term, const char *payload)
{
    if (s_state_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    room->valid = true;
    room->occupied = json_bool(payload, "occupied", false);
    room->light = json_bool(payload, "light", false);
    json_str(payload, "mode", room->mode, sizeof(room->mode));
    room->seq = json_u32(payload, "seq", 0);
    bool occupied = room->occupied;
    bool light = room->light;
    char mode[8];
    strlcpy(mode, room->mode, sizeof(mode));
    xSemaphoreGive(s_state_lock);
    ESP_LOGI(TAG, "%s state: occupied=%d light=%d mode=%s", term, occupied, light, mode);
}

static void update_room_online(room_state_t *room, const char *term, const char *payload)
{
    bool online = strncmp(payload, "online", 6) == 0;
    if (s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        room->online = online;
        xSemaphoreGive(s_state_lock);
    }
    ESP_LOGI(TAG, "%s is %s", term, online ? "online" : "offline");
}

static void on_mqtt_message(const char *topic, const char *payload, int len)
{
    (void)len;
    if (strcmp(topic, TOPIC_ROOM1_STATE) == 0) {
        update_room(&s_room1, "ROOM1", payload);
    } else if (strcmp(topic, TOPIC_ROOM2_STATE) == 0) {
        update_room(&s_room2, "ROOM2", payload);
    } else if (strcmp(topic, TOPIC_ROOM1_STATUS) == 0) {
        update_room_online(&s_room1, "ROOM1", payload);
    } else if (strcmp(topic, TOPIC_ROOM2_STATUS) == 0) {
        update_room_online(&s_room2, "ROOM2", payload);
    }
}

static void nvs_setup(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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

static void config_load(void)
{
    s_sched_start_min = ACCESS_START_HOUR_DEFAULT * 60;
    s_sched_end_min = ACCESS_END_HOUR_DEFAULT * 60;
    strlcpy(s_admin_pin, ADMIN_PIN_DEFAULT, sizeof(s_admin_pin));
    if (!s_nvs_ok) {
        return;
    }
    uint16_t v = 0;
    if (nvs_get_u16(s_nvs, "start", &v) == ESP_OK) {
        s_sched_start_min = v;
    }
    if (nvs_get_u16(s_nvs, "end", &v) == ESP_OK) {
        s_sched_end_min = v;
    }
    size_t len = sizeof(s_admin_pin);
    if (nvs_get_str(s_nvs, "pin", s_admin_pin, &len) != ESP_OK) {
        strlcpy(s_admin_pin, ADMIN_PIN_DEFAULT, sizeof(s_admin_pin));
    }
}

static void config_save(void)
{
    if (!s_nvs_ok) {
        return;
    }
    nvs_set_u16(s_nvs, "start", (uint16_t)s_sched_start_min);
    nvs_set_u16(s_nvs, "end", (uint16_t)s_sched_end_min);
    nvs_set_str(s_nvs, "pin", s_admin_pin);
    nvs_commit(s_nvs);
}

static void gpio_setup(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << DOOR_LOCK_GPIO) | (1ULL << BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(DOOR_LOCK_GPIO, 0);
    gpio_set_level(BUZZER_GPIO, 0);
}

static void sd_setup(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 4000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI3_HOST;
    slot.gpio_cs = SD_CS_GPIO;

    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return;
    }
    s_sd_ok = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
}

static void rfid_bus_setup(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = RFID_MOSI_GPIO,
        .miso_io_num = RFID_MISO_GPIO,
        .sclk_io_num = RFID_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
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

static void time_setup(void)
{
    setenv("TZ", "EAT-3", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void buzzer_beep(int times, int ms)
{
    for (int i = 0; i < times; i++) {
        int cycles = ms / 2;
        if (cycles < 1) {
            cycles = 1;
        }
        for (int c = 0; c < cycles; c++) {
            gpio_set_level(BUZZER_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1));
            gpio_set_level(BUZZER_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (i + 1 < times) {
            vTaskDelay(pdMS_TO_TICKS(80));
        }
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

static const allowed_user_t *find_user(const char *uid)
{
    for (size_t i = 0; i < sizeof(s_allowed) / sizeof(s_allowed[0]); i++) {
        if (strcmp(s_allowed[i].uid, uid) == 0) {
            return &s_allowed[i];
        }
    }
    return NULL;
}

static int find_occupant(const char *uid)
{
    for (int i = 0; i < s_occupant_count; i++) {
        if (strcmp(s_occupants[i].uid, uid) == 0) {
            return i;
        }
    }
    return -1;
}

static bool add_occupant(const char *uid, const char *name)
{
    if (s_occupant_count >= MAX_PEOPLE) {
        return false;
    }
    strlcpy(s_occupants[s_occupant_count].uid, uid, sizeof(s_occupants[0].uid));
    strlcpy(s_occupants[s_occupant_count].name, name, sizeof(s_occupants[0].name));
    s_occupants[s_occupant_count].entered = time(NULL);
    s_occupant_count++;
    return true;
}

static void remove_occupant(int idx)
{
    for (int i = idx; i < s_occupant_count - 1; i++) {
        s_occupants[i] = s_occupants[i + 1];
    }
    s_occupant_count--;
}

static void sd_log_line(const char *line)
{
    if (!s_sd_ok) {
        return;
    }
    if (s_sd_lock == NULL || xSemaphoreTake(s_sd_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }
    FILE *f = fopen(SD_LOG_FILE, "a");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            fprintf(f, "timestamp,event,uid,name,inside,result\n");
        }
        fprintf(f, "%s\n", line);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "failed to open %s", SD_LOG_FILE);
    }
    xSemaphoreGive(s_sd_lock);
}

static void log_access(const char *event, const char *uid, const char *name, int inside,
                       const char *result)
{
    char ts[32];
    fmt_time(ts, sizeof(ts));
    char line[192];
    snprintf(line, sizeof(line), "%s,%s,%s,%s,%d,%s", ts, event, uid, name, inside, result);
    ESP_LOGI(TAG, "%s", line);
    sd_log_line(line);
}

static void publish_event(const char *type, const char *uid, const char *name,
                          const char *result, int inside)
{
    char ts[32];
    fmt_time(ts, sizeof(ts));
    char payload[360];
    uint32_t seq = __atomic_add_fetch(&s_event_seq, 1, __ATOMIC_RELAXED);
    snprintf(payload, sizeof(payload),
             "{\"term\":\"entrance\",\"type\":\"%s\",\"uid\":\"%s\",\"name\":\"%s\","
             "\"result\":\"%s\",\"inside\":%d,\"seq\":%u,\"ts\":\"%s\"}",
             type, uid, name, result, inside, (unsigned)seq, ts);
    mqtt_bus_publish(TOPIC_ENTRANCE_EVENT, payload, (int)strlen(payload), false);
}

static void publish_state(void)
{
    char ts[32];
    char start[8];
    char end[8];
    fmt_time(ts, sizeof(ts));
    fmt_hhmm(s_sched_start_min, start, sizeof(start));
    fmt_hhmm(s_sched_end_min, end, sizeof(end));

    int inside = 0;
    if (s_state_lock != NULL && xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
        inside = s_occupant_count;
        xSemaphoreGive(s_state_lock);
    }

    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"term\":\"entrance\",\"inside\":%d,\"capacity\":%d,\"schedule\":\"%s-%s\","
             "\"time\":\"%s\",\"synced\":%s,\"sd\":%s}",
             inside, MAX_PEOPLE, start, end, ts, time_valid() ? "true" : "false",
             s_sd_ok ? "true" : "false");
    mqtt_bus_publish(TOPIC_ENTRANCE_STATE, payload, (int)strlen(payload), true);
}

static void handle_entry(const char *uid)
{
    bool granted = false;
    const char *result = "DENIED_UNKNOWN";
    const allowed_user_t *user = find_user(uid);
    const char *name = user != NULL ? user->name : "Unknown";
    int inside = 0;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (user == NULL) {
        result = "DENIED_UNKNOWN";
    } else if (!access_window_open()) {
        result = "DENIED_SCHEDULE";
    } else if (find_occupant(uid) >= 0) {
        result = "DENIED_DUPLICATE";
    } else if (s_occupant_count >= MAX_PEOPLE) {
        result = "DENIED_CAPACITY";
    } else if (add_occupant(uid, user->name)) {
        granted = true;
        result = "GRANTED";
    } else {
        result = "DENIED_CAPACITY";
    }
    inside = s_occupant_count;
    xSemaphoreGive(s_state_lock);

    log_access("ENTRY", uid, name, inside, result);
    publish_event("ENTRY", uid, name, result, inside);
    if (granted) {
        feedback_grant();
    } else {
        feedback_deny();
    }
    publish_state();
}

static void handle_exit(const char *uid)
{
    const allowed_user_t *user = find_user(uid);
    const char *name = user != NULL ? user->name : "Unknown";
    bool granted = false;
    const char *result = "DENIED_NOT_INSIDE";
    int inside = 0;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    int idx = find_occupant(uid);
    if (idx >= 0) {
        remove_occupant(idx);
        granted = true;
        result = "GRANTED";
    }
    inside = s_occupant_count;
    xSemaphoreGive(s_state_lock);

    log_access("EXIT", uid, name, inside, result);
    publish_event("EXIT", uid, name, result, inside);
    if (granted) {
        feedback_grant();
    } else {
        feedback_deny();
    }
    publish_state();
}

static void rfid_task(void *arg)
{
    reader_ctx_t *ctx = (reader_ctx_t *)arg;
    char last_uid[24] = "";
    bool present = false;
    int absent_reads = 0;
    uint8_t shown_stage = 0xFF;

    ESP_LOGI(TAG, "%s RFID task started (CS=%d)", ctx->term, (int)ctx->reader->cs);

    while (true) {
        uint8_t uid[10];
        uint8_t len = 0;
        if (rc522_read_uid(ctx->reader, uid, &len)) {
            char u[24];
            uid_to_str(uid, len, u, sizeof(u));
            absent_reads = 0;
            shown_stage = 0;
            if (!present || strcmp(u, last_uid) != 0) {
                present = true;
                strlcpy(last_uid, u, sizeof(last_uid));
                ESP_LOGI(TAG, "%s card %s", ctx->term, u);
                if (ctx->entry) {
                    handle_entry(u);
                } else {
                    handle_exit(u);
                }
            }
        } else {
            if (ctx->reader->last_stage != shown_stage) {
                shown_stage = ctx->reader->last_stage;
                ESP_LOGW(TAG, "%s stage=%u atqa=%02X%02X/%u resp=%02X%02X%02X%02X%02X/%u",
                         ctx->term, (unsigned)shown_stage,
                         ctx->reader->last_atqa[0], ctx->reader->last_atqa[1],
                         (unsigned)ctx->reader->last_atqa_len,
                         ctx->reader->last_resp[0], ctx->reader->last_resp[1],
                         ctx->reader->last_resp[2], ctx->reader->last_resp[3],
                         ctx->reader->last_resp[4],
                         (unsigned)ctx->reader->last_resp_len);
            }
            if (present && ++absent_reads >= 8) {
                present = false;
                last_uid[0] = '\0';
            }
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static bool is_authenticated(void)
{
    if (!s_authenticated) {
        return false;
    }
    if ((int32_t)(s_auth_until - xTaskGetTickCount()) > 0) {
        return true;
    }
    s_authenticated = false;
    return false;
}

static void auth_ok(void)
{
    s_authenticated = true;
    s_auth_until = xTaskGetTickCount() + pdMS_TO_TICKS(AUTH_WINDOW_MS);
}

static void print_help(void)
{
    printf("\nCoCIS Entrance commands:\n");
    printf("  status                     show hub, room and network state\n");
    printf("  list                       list people currently inside\n");
    printf("  users                      show the RFID allow-list\n");
    printf("  logs [n]                   show the last n SD log lines\n");
    printf("  login <pin>                unlock admin commands for 60 s\n");
    printf("  logout                     lock admin commands\n");
    printf("  schedule                   show the access schedule\n");
    printf("  schedule <HH:MM> <HH:MM>   change the schedule (admin)\n");
    printf("  settime YYYY-MM-DD HH:MM:SS set the clock (admin)\n");
    printf("  setpin <old> <new>         change the admin PIN\n");
    printf("  room1 on|off|auto|status   control Room 1 lighting\n");
    printf("  room2 on|off|auto|status   control Room 2 lighting\n");
    printf("  mqtt                       show MQTT connection state\n");
    printf("  reboot                     restart the controller (admin)\n");
    printf("  help                       show this help\n\n");
    fflush(stdout);
}

static void print_status(void)
{
    char ts[32];
    char start[8];
    char end[8];
    fmt_time(ts, sizeof(ts));
    fmt_hhmm(s_sched_start_min, start, sizeof(start));
    fmt_hhmm(s_sched_end_min, end, sizeof(end));

    printf("\n=== CoCIS Entrance Hub ===\n");
    printf("Time       : %s\n", ts);
    printf("Schedule   : %s-%s (%s)\n", start, end, access_window_open() ? "OPEN" : "CLOSED");
    printf("MQTT       : %s\n", mqtt_bus_connected() ? "connected" : "offline");
    printf("SD card    : %s\n", s_sd_ok ? "mounted" : "not mounted");
    printf("Admin      : %s\n", is_authenticated() ? "unlocked" : "locked");

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    printf("Inside     : %d/%d\n", s_occupant_count, MAX_PEOPLE);
    for (int i = 0; i < s_occupant_count; i++) {
        char when[32];
        struct tm tm_in;
        localtime_r(&s_occupants[i].entered, &tm_in);
        strftime(when, sizeof(when), "%H:%M:%S", &tm_in);
        printf("  %2d. %-22s %s since %s\n", i + 1, s_occupants[i].name,
               s_occupants[i].uid, when);
    }

    printf("Room 1     : %s", s_room1.valid ? "" : "no data\n");
    if (s_room1.valid) {
        printf("online=%d occupied=%d light=%d mode=%s seq=%u\n", s_room1.online,
               s_room1.occupied, s_room1.light, s_room1.mode, (unsigned)s_room1.seq);
    }
    printf("Room 2     : %s", s_room2.valid ? "" : "no data\n");
    if (s_room2.valid) {
        printf("online=%d occupied=%d light=%d mode=%s seq=%u\n", s_room2.online,
               s_room2.occupied, s_room2.light, s_room2.mode, (unsigned)s_room2.seq);
    }
    xSemaphoreGive(s_state_lock);
    printf("==========================\n\n");
    fflush(stdout);
}

static void print_users(void)
{
    printf("\nRFID allow-list:\n");
    for (size_t i = 0; i < sizeof(s_allowed) / sizeof(s_allowed[0]); i++) {
        printf("  %-24s %s\n", s_allowed[i].uid, s_allowed[i].name);
    }
    printf("  All other cards are denied.\n\n");
    fflush(stdout);
}

static void tail_logs(int n)
{
    if (!s_sd_ok) {
        printf("SD card is not mounted.\n");
        fflush(stdout);
        return;
    }
    if (n <= 0) {
        n = 10;
    }
    if (n > 50) {
        n = 50;
    }

    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    FILE *f = fopen(SD_LOG_FILE, "r");
    if (f == NULL) {
        xSemaphoreGive(s_sd_lock);
        printf("No log file yet.\n");
        fflush(stdout);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long start = size > 4096 ? size - 4096 : 0;
    fseek(f, start, SEEK_SET);
    char buf[4097];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    buf[rd] = '\0';
    fclose(f);
    xSemaphoreGive(s_sd_lock);

    char *p = buf;
    if (start > 0) {
        char *nl = strchr(buf, '\n');
        if (nl != NULL) {
            p = nl + 1;
        }
    }

    char *lines[64];
    int count = 0;
    char *save = NULL;
    char *tok = strtok_r(p, "\n", &save);
    while (tok != NULL && count < 64) {
        lines[count++] = tok;
        tok = strtok_r(NULL, "\n", &save);
    }
    int from = count > n ? count - n : 0;
    printf("\nLast %d log line(s):\n", count - from);
    for (int i = from; i < count; i++) {
        printf("%s\n", lines[i]);
    }
    printf("\n");
    fflush(stdout);
}

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

static void handle_menu_line(char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                       line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    char *cmd = line;
    char *rest = strchr(line, ' ');
    if (rest != NULL) {
        *rest++ = '\0';
        while (*rest == ' ') {
            rest++;
        }
    } else {
        rest = line + strlen(line);
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0) {
        print_status();
    } else if (strcmp(cmd, "list") == 0) {
        print_status();
    } else if (strcmp(cmd, "users") == 0) {
        print_users();
    } else if (strcmp(cmd, "logs") == 0) {
        tail_logs(*rest != '\0' ? atoi(rest) : 10);
    } else if (strcmp(cmd, "login") == 0) {
        if (*rest != '\0' && strcmp(rest, s_admin_pin) == 0) {
            auth_ok();
            printf("Admin unlocked for 60 s.\n");
        } else {
            log_access("AUTH", "-", "admin", s_occupant_count, "DENIED_PIN");
            printf("Wrong PIN.\n");
        }
        fflush(stdout);
    } else if (strcmp(cmd, "logout") == 0) {
        s_authenticated = false;
        printf("Admin locked.\n");
        fflush(stdout);
    } else if (strcmp(cmd, "schedule") == 0) {
        char start[8];
        char end[8];
        fmt_hhmm(s_sched_start_min, start, sizeof(start));
        fmt_hhmm(s_sched_end_min, end, sizeof(end));
        if (*rest == '\0') {
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
        if (sscanf(rest, "%d:%d %d:%d", &h1, &m1, &h2, &m2) != 4 ||
            h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23 ||
            m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) {
            printf("Usage: schedule HH:MM HH:MM\n");
            fflush(stdout);
            return;
        }
        s_sched_start_min = h1 * 60 + m1;
        s_sched_end_min = h2 * 60 + m2;
        config_save();
        fmt_hhmm(s_sched_start_min, start, sizeof(start));
        fmt_hhmm(s_sched_end_min, end, sizeof(end));
        printf("Schedule saved: %s-%s\n", start, end);
        publish_state();
        fflush(stdout);
    } else if (strcmp(cmd, "settime") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }
        int Y, M, D, h, m, s;
        if (sscanf(rest, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6 ||
            Y < 2020 || M < 1 || M > 12 || D < 1 || D > 31 ||
            h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) {
            printf("Usage: settime YYYY-MM-DD HH:MM:SS\n");
            fflush(stdout);
            return;
        }
        struct tm tm_set = {
            .tm_year = Y - 1900,
            .tm_mon = M - 1,
            .tm_mday = D,
            .tm_hour = h,
            .tm_min = m,
            .tm_sec = s,
            .tm_isdst = -1,
        };
        time_t t = mktime(&tm_set);
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        esp_sntp_stop();
        char ts[32];
        fmt_time(ts, sizeof(ts));
        printf("Clock set to %s (NTP disabled until reboot)\n", ts);
        publish_state();
        fflush(stdout);
    } else if (strcmp(cmd, "setpin") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }
        char old_pin[8] = "";
        char new_pin[8] = "";
        if (sscanf(rest, "%7s %7s", old_pin, new_pin) != 2) {
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
    } else if (strcmp(cmd, "room1") == 0 || strcmp(cmd, "room2") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }
        int room = cmd[4] - '0';
        const char *topic = room == 1 ? TOPIC_ROOM1_CMD : TOPIC_ROOM2_CMD;
        char action[16] = "";
        if (sscanf(rest, "%15s", action) != 1) {
            printf("Usage: %s on|off|auto|status\n", cmd);
            fflush(stdout);
            return;
        }
        for (int i = 0; action[i] != '\0'; i++) {
            if (action[i] >= 'a' && action[i] <= 'z') {
                action[i] = (char)(action[i] - 'a' + 'A');
            }
        }
        if (strcmp(action, "ON") != 0 && strcmp(action, "OFF") != 0 &&
            strcmp(action, "AUTO") != 0 && strcmp(action, "STATUS") != 0) {
            printf("Usage: %s on|off|auto|status\n", cmd);
            fflush(stdout);
            return;
        }
        if (mqtt_bus_publish(topic, action, (int)strlen(action), false) == 0) {
            printf("Sent %s to Room %d.\n", action, room);
        } else {
            printf("MQTT is offline; command not sent.\n");
        }
        fflush(stdout);
    } else if (strcmp(cmd, "mqtt") == 0) {
        printf("MQTT %s:%d, client %s, prefix %s (%s)\n", MQTT_HOST, MQTT_PORT,
               MQTT_CLIENT_ID, MQTT_TOPIC_PREFIX,
               mqtt_bus_connected() ? "connected" : "offline");
        fflush(stdout);
    } else if (strcmp(cmd, "reboot") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }
        printf("Rebooting...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else {
        printf("Unknown command '%s'. Type help.\n", cmd);
        fflush(stdout);
    }
}

static void menu_task(void *arg)
{
    (void)arg;
    char line[160];
    size_t len = 0;
    bool last_cr = false;

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

        if (c == '\r' || c == '\n') {
            if (c == '\n' && last_cr) {
                last_cr = false;
                continue;
            }
            last_cr = (c == '\r');
            printf("\n");
            if (len > 0) {
                line[len] = '\0';
                handle_menu_line(line);
                len = 0;
            }
            printf("cocis> ");
            fflush(stdout);
            continue;
        }

        last_cr = false;
        if (c == '\b' || c == 0x7F) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }

        if (c >= 0x20 && c < 0x7F && len + 1 < sizeof(line)) {
            line[len++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
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
    ESP_LOGI(TAG, "CoCIS Entrance Hub starting");

    s_state_lock = xSemaphoreCreateMutex();
    s_sd_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL || s_sd_lock == NULL) {
        ESP_LOGE(TAG, "failed to create mutexes");
        return;
    }

    nvs_setup();
    config_load();
    gpio_setup();
    sd_setup();
    rfid_bus_setup();

    bool entry_ok = rc522_init(&s_entry_reader, SPI2_HOST, RFID_ENTRY_CS_GPIO,
                               RFID_ENTRY_RST_GPIO) == ESP_OK;
    bool exit_ok = rc522_init(&s_exit_reader, SPI2_HOST, RFID_EXIT_CS_GPIO,
                              RFID_EXIT_RST_GPIO) == ESP_OK;
    if (!entry_ok) {
        ESP_LOGE(TAG, "entry reader init failed");
    }
    if (!exit_ok) {
        ESP_LOGE(TAG, "exit reader init failed");
    }

    wifi_setup();
    bool wifi_ok = wifi_wait_connected(30000);
    ESP_LOGI(TAG, "Wi-Fi %s", wifi_ok ? "connected" : "not connected, continuing offline");

    time_setup();
    mqtt_bus_start(MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID, MQTT_STATUS_TOPIC, "offline",
                   on_mqtt_message);
    mqtt_bus_subscribe(TOPIC_ROOM1_STATE);
    mqtt_bus_subscribe(TOPIC_ROOM2_STATE);
    mqtt_bus_subscribe(TOPIC_ROOM1_STATUS);
    mqtt_bus_subscribe(TOPIC_ROOM2_STATUS);

    if (entry_ok) {
        xTaskCreate(rfid_task, "rfid_entry", 4096, &s_entry_ctx, 5, NULL);
    }
    if (exit_ok) {
        xTaskCreate(rfid_task, "rfid_exit", 4096, &s_exit_ctx, 5, NULL);
    }
    xTaskCreate(menu_task, "menu", 8192, NULL, 3, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 3072, NULL, 2, NULL);

    publish_state();
    ESP_LOGI(TAG, "ready: tap a card and use the serial menu");
}
