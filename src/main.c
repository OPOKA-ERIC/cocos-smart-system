#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdmmc_cmd.h"

#include "config.h"
#include "rc522.h"

static const char *TAG = "COCIS";

typedef struct {
    char uid[24];
    char name[24];
} allowed_user_t;

typedef struct {
    char uid[24];
    char name[24];
    time_t entered;
} occupant_t;

static const allowed_user_t s_allowed[] = {
    {"01:02:03:04", "Blue Card (Alice)"},
    {"55:66:77:88", "Yellow Card (Bob)"},
    {"AA:BB:CC:DD", "Red Card (Carol)"},
    {"04:11:22:33:44:55:66", "NFC Tag (Dave)"},
    {"04:11:22:33", "NFC Tag (Dave)"},
};

static rc522_t s_entry_reader;
static rc522_t s_exit_reader;
static bool s_entry_ok = false;
static bool s_exit_ok = false;
static SemaphoreHandle_t s_state_lock = NULL;
static SemaphoreHandle_t s_sd_lock = NULL;
static nvs_handle_t s_nvs;
static bool s_nvs_ok = false;
static sdmmc_card_t *s_card = NULL;
static bool s_sd_ok = false;

static int s_sched_start_min = ACCESS_START_HOUR_DEFAULT * 60;
static int s_sched_end_min = ACCESS_END_HOUR_DEFAULT * 60;
static char s_admin_pin[12] = ADMIN_PIN_DEFAULT;

static bool s_authenticated = false;
static TickType_t s_auth_until = 0;

static occupant_t s_occupants[MAX_PEOPLE];
static int s_occupant_count = 0;

// Rooms
static SemaphoreHandle_t s_room_lock = NULL;
static bool s_room1_occupied = false;
static bool s_room2_occupied = false;
static bool s_room1_light = false;
static bool s_room2_light = false;
static char s_room1_mode[8] = "AUTO";
static char s_room2_mode[8] = "AUTO";
static TickType_t s_room1_last_motion = 0;
static TickType_t s_room2_last_motion = 0;

static void fmt_time(char *out, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 100) {
        strlcpy(out, "1970-01-01 00:00:00", len);
        return;
    }
    strftime(out, len, "%Y-%m-%d %H:%M:%S", &tm_now);
}

static void fmt_hhmm(int mins, char *out, size_t len)
{
    snprintf(out, len, "%02d:%02d", mins / 60, mins % 60);
}

static bool access_window_open(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 100) {
        return true; // fail open if time not set
    }
    int cur = tm_now.tm_hour * 60 + tm_now.tm_min;
    if (s_sched_start_min <= s_sched_end_min) {
        return cur >= s_sched_start_min && cur <= s_sched_end_min;
    }
    return cur >= s_sched_start_min || cur <= s_sched_end_min;
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
    if (!s_nvs_ok) return;
    uint16_t v = 0;
    if (nvs_get_u16(s_nvs, "start", &v) == ESP_OK) s_sched_start_min = v;
    if (nvs_get_u16(s_nvs, "end", &v) == ESP_OK) s_sched_end_min = v;
    size_t len = sizeof(s_admin_pin);
    if (nvs_get_str(s_nvs, "pin", s_admin_pin, &len) != ESP_OK) {
        strlcpy(s_admin_pin, ADMIN_PIN_DEFAULT, sizeof(s_admin_pin));
    }
}

static void config_save(void)
{
    if (!s_nvs_ok) return;
    nvs_set_u16(s_nvs, "start", (uint16_t)s_sched_start_min);
    nvs_set_u16(s_nvs, "end", (uint16_t)s_sched_end_min);
    nvs_set_str(s_nvs, "pin", s_admin_pin);
    nvs_commit(s_nvs);
}

static void gpio_setup(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << DOOR_LOCK_GPIO) | (1ULL << BUZZER_GPIO) | (1ULL << ROOM1_LIGHT_GPIO) | (1ULL << ROOM2_LIGHT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(DOOR_LOCK_GPIO, 0);
    gpio_set_level(BUZZER_GPIO, 0);
    gpio_set_level(ROOM1_LIGHT_GPIO, 0);
    gpio_set_level(ROOM2_LIGHT_GPIO, 0);

    gpio_config_t pir = {
        .pin_bit_mask = (1ULL << ROOM1_PIR_GPIO) | (1ULL << ROOM2_PIR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pir));
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

static void sd_log_line(const char *line)
{
    if (!s_sd_ok) return;
    if (s_sd_lock == NULL || xSemaphoreTake(s_sd_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    FILE *f = fopen(SD_LOG_FILE, "a");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) == 0) {
            fprintf(f, "timestamp,event,uid,name,inside,result\n");
        }
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    xSemaphoreGive(s_sd_lock);
}

static void log_access(const char *event, const char *uid, const char *name, int inside, const char *result)
{
    char ts[32];
    fmt_time(ts, sizeof(ts));
    char line[192];
    snprintf(line, sizeof(line), "%s,%s,%s,%s,%d,%s", ts, event, uid, name, inside, result);
    ESP_LOGI(TAG, "%s", line);
    sd_log_line(line);
}

static const allowed_user_t *find_user(const char *uid)
{
    for (size_t i = 0; i < sizeof(s_allowed)/sizeof(s_allowed[0]); i++) {
        if (strcmp(s_allowed[i].uid, uid) == 0) return &s_allowed[i];
    }
    return NULL;
}

static int find_occupant(const char *uid)
{
    for (int i = 0; i < s_occupant_count; i++) {
        if (strcmp(s_occupants[i].uid, uid) == 0) return i;
    }
    return -1;
}

static bool add_occupant(const char *uid, const char *name)
{
    if (s_occupant_count >= MAX_PEOPLE) return false;
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

static void buzzer_beep(int times, int ms)
{
    for (int i = 0; i < times; i++) {
        int cycles = ms / 2;
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
    if (granted) feedback_grant();
    else feedback_deny();
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
    if (granted) feedback_grant();
    else feedback_deny();
}

static void rfid_task(void *arg)
{
    rc522_t *reader = (rc522_t *)arg;
    bool is_entry = (reader == &s_entry_reader);
    char last_uid[24] = "";
    bool present = false;
    int absent_reads = 0;

    while (true) {
        uint8_t uid[10];
        uint8_t len = 0;
        if (rc522_read_uid(reader, uid, &len)) {
            char u[48];
            u[0] = '\0';
            for (int i = 0; i < len && i < 10; i++) {
                char tmp[6];
                snprintf(tmp, sizeof(tmp), "%02X%s", uid[i], i < len - 1 ? ":" : "");
                strlcat(u, tmp, sizeof(u));
            }
            absent_reads = 0;
            if (!present || strcmp(u, last_uid) != 0) {
                present = true;
                strlcpy(last_uid, u, sizeof(last_uid));
                ESP_LOGI(TAG, "%s card %s", is_entry ? "ENTRY" : "EXIT", u);
                if (is_entry) handle_entry(u);
                else handle_exit(u);
            }
        } else {
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
    if (!s_authenticated) return false;
    if ((int32_t)(s_auth_until - xTaskGetTickCount()) > 0) return true;
    s_authenticated = false;
    return false;
}

static void auth_ok(void)
{
    s_authenticated = true;
    s_auth_until = xTaskGetTickCount() + pdMS_TO_TICKS(AUTH_WINDOW_MS);
}

static bool valid_pin(const char *pin)
{
    size_t len = strlen(pin);
    if (len < 4 || len > 7) return false;
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return true;
}

// Room control (local)
static void room_set_mode(int room, const char *mode)
{
    if (strcmp(mode, "ON") != 0 && strcmp(mode, "OFF") != 0 && strcmp(mode, "AUTO") != 0) return;
    if (room == 1) {
        xSemaphoreTake(s_room_lock, portMAX_DELAY);
        strlcpy(s_room1_mode, mode, sizeof(s_room1_mode));
        if (strcmp(mode, "ON") == 0) {
            s_room1_light = true;
        } else if (strcmp(mode, "OFF") == 0) {
            s_room1_light = false;
        } else {
            s_room1_light = s_room1_occupied;
        }
        xSemaphoreGive(s_room_lock);
        gpio_set_level(ROOM1_LIGHT_GPIO, s_room1_light ? 1 : 0);
        ESP_LOGI(TAG, "ROOM1 mode=%s light=%d", mode, s_room1_light);
    } else if (room == 2) {
        xSemaphoreTake(s_room_lock, portMAX_DELAY);
        strlcpy(s_room2_mode, mode, sizeof(s_room2_mode));
        if (strcmp(mode, "ON") == 0) {
            s_room2_light = true;
        } else if (strcmp(mode, "OFF") == 0) {
            s_room2_light = false;
        } else {
            s_room2_light = s_room2_occupied;
        }
        xSemaphoreGive(s_room_lock);
        gpio_set_level(ROOM2_LIGHT_GPIO, s_room2_light ? 1 : 0);
        ESP_LOGI(TAG, "ROOM2 mode=%s light=%d", mode, s_room2_light);
    }
}

static void room_task(void *arg)
{
    (void)arg;
    s_room1_last_motion = xTaskGetTickCount();
    s_room2_last_motion = xTaskGetTickCount();
    while (true) {
        TickType_t now = xTaskGetTickCount();
        int p1 = gpio_get_level(ROOM1_PIR_GPIO);
        int p2 = gpio_get_level(ROOM2_PIR_GPIO);

        xSemaphoreTake(s_room_lock, portMAX_DELAY);
    bool changed = false;
    if (p1) {
        s_room1_last_motion = now;
        if (!s_room1_occupied) { s_room1_occupied = true; changed = true; }
    } else if (s_room1_occupied && (now - s_room1_last_motion) > pdMS_TO_TICKS(OCCUPANCY_TIMEOUT_MS)) {
        s_room1_occupied = false; changed = true;
    }
    bool l1_old = s_room1_light;
    if (strcmp(s_room1_mode, "ON") == 0) {
        s_room1_light = true;
    } else if (strcmp(s_room1_mode, "OFF") == 0) {
        s_room1_light = false;
    } else {
        s_room1_light = s_room1_occupied;
    }
    if (l1_old != s_room1_light) changed = true;

    if (p2) {
        s_room2_last_motion = now;
        if (!s_room2_occupied) { s_room2_occupied = true; changed = true; }
    } else if (s_room2_occupied && (now - s_room2_last_motion) > pdMS_TO_TICKS(OCCUPANCY_TIMEOUT_MS)) {
        s_room2_occupied = false; changed = true;
    }
    bool l2_old = s_room2_light;
    if (strcmp(s_room2_mode, "ON") == 0) {
        s_room2_light = true;
    } else if (strcmp(s_room2_mode, "OFF") == 0) {
        s_room2_light = false;
    } else {
        s_room2_light = s_room2_occupied;
    }
    if (l2_old != s_room2_light) changed = true;
    xSemaphoreGive(s_room_lock);

    gpio_set_level(ROOM1_LIGHT_GPIO, s_room1_light ? 1 : 0);
    gpio_set_level(ROOM2_LIGHT_GPIO, s_room2_light ? 1 : 0);
    if (changed) {
        ESP_LOGI(TAG, "ROOM1: occ=%d light=%d mode=%s | ROOM2: occ=%d light=%d mode=%s", s_room1_occupied, s_room1_light, s_room1_mode, s_room2_occupied, s_room2_light, s_room2_mode);
    }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void print_help(void)
{
    printf("\nCoCIS Single-ESP32 commands:\n");
    printf("  status                     show hub, rooms and state\n");
    printf("  list                       list people currently inside\n");
    printf("  users                      show the RFID allow-list\n");
    printf("  logs [n]                   show the last n SD log lines\n");
    printf("  login <pin>                unlock admin commands for 60 s\n");
    printf("  logout                     lock admin commands\n");
    printf("  schedule                   show the access schedule\n");
    printf("  schedule <HH:MM> <HH:MM>   change the schedule (admin)\n");
    printf("  settime YYYY-MM-DD HH:MM:SS set the clock (admin)\n");
    printf("  setpin <old> <new>         change the admin PIN (admin)\n");
    printf("  room1 on|off|auto|status   control Room 1 lighting (admin)\n");
    printf("  room2 on|off|auto|status   control Room 2 lighting (admin)\n");
    printf("  reboot                     restart the controller (admin)\n");
    printf("  help                       show this help\n\n");
    fflush(stdout);
}

static void print_status(void)
{
    char ts[32];
    char start[8], end[8];
    fmt_time(ts, sizeof(ts));
    fmt_hhmm(s_sched_start_min, start, sizeof(start));
    fmt_hhmm(s_sched_end_min, end, sizeof(end));

    printf("\n=== CoCIS Single ESP32 ===\n");
    printf("Time       : %s\n", ts);
    printf("Schedule   : %s-%s (%s)\n", start, end, access_window_open() ? "OPEN" : "CLOSED");
    printf("SD card    : %s\n", s_sd_ok ? "mounted" : "not mounted");
    printf("Admin      : %s\n", is_authenticated() ? "unlocked" : "locked");

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    printf("Inside     : %d/%d\n", s_occupant_count, MAX_PEOPLE);
    for (int i = 0; i < s_occupant_count; i++) {
        char when[32];
        struct tm tm_in;
        localtime_r(&s_occupants[i].entered, &tm_in);
        strftime(when, sizeof(when), "%H:%M:%S", &tm_in);
        printf("  %2d. %-22s %s since %s\n", i + 1, s_occupants[i].name, s_occupants[i].uid, when);
    }
    xSemaphoreGive(s_state_lock);

    xSemaphoreTake(s_room_lock, portMAX_DELAY);
    printf("Room 1     : occupied=%d light=%d mode=%s\n", s_room1_occupied, s_room1_light, s_room1_mode);
    printf("Room 2     : occupied=%d light=%d mode=%s\n", s_room2_occupied, s_room2_light, s_room2_mode);
    xSemaphoreGive(s_room_lock);
    printf("==========================\n\n");
    fflush(stdout);
}

static void print_users(void)
{
    printf("\nRFID allow-list:\n");
    for (size_t i = 0; i < sizeof(s_allowed)/sizeof(s_allowed[0]); i++) {
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
    if (n <= 0) n = 10;
    if (n > 50) n = 50;

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
        if (nl != NULL) p = nl + 1;
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
    for (int i = from; i < count; i++) printf("%s\n", lines[i]);
    printf("\n");
    fflush(stdout);
}

static void handle_menu_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' || line[len-1] == ' ' || line[len-1] == '\t')) line[--len] = '\0';
    if (len == 0) return;

    char *cmd = line;
    char *rest = strchr(line, ' ');
    if (rest != NULL) {
        *rest++ = '\0';
        while (*rest == ' ') rest++;
    } else {
        rest = line + strlen(line);
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "status") == 0 || strcmp(cmd, "list") == 0) {
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
        char start[8], end[8];
        fmt_hhmm(s_sched_start_min, start, sizeof(start));
        fmt_hhmm(s_sched_end_min, end, sizeof(end));
        if (*rest == '\0') {
            printf("Access schedule: %s-%s (%s)\n", start, end, access_window_open() ? "OPEN" : "CLOSED");
            fflush(stdout);
            return;
        }
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }
        int h1,m1,h2,m2;
        if (sscanf(rest, "%d:%d %d:%d", &h1,&m1,&h2,&m2) != 4 || h1<0||h1>23||h2<0||h2>23||m1<0||m1>59||m2<0||m2>59) {
            printf("Usage: schedule HH:MM HH:MM\n");
            fflush(stdout);
            return;
        }
        s_sched_start_min = h1*60+m1;
        s_sched_end_min = h2*60+m2;
        config_save();
        printf("Schedule saved: %02d:%02d-%02d:%02d\n", h1,m1,h2,m2);
        fflush(stdout);
    } else if (strcmp(cmd, "settime") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }
        int Y,M,D,h,m,s;
        if (sscanf(rest, "%d-%d-%d %d:%d:%d", &Y,&M,&D,&h,&m,&s) != 6 || Y<2020||M<1||M>12||D<1||D>31||h<0||h>23||m<0||m>59||s<0||s>59) {
            printf("Usage: settime YYYY-MM-DD HH:MM:SS\n");
            fflush(stdout);
            return;
        }
        struct tm tm_set = {.tm_year = Y-1900, .tm_mon = M-1, .tm_mday = D, .tm_hour = h, .tm_min = m, .tm_sec = s, .tm_isdst = -1};
        time_t t = mktime(&tm_set);
        if (t < 0) t = time(NULL);
        struct timeval tv = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&tv, NULL);
        char ts[32];
        fmt_time(ts, sizeof(ts));
        printf("Clock set to %s (NTP disabled)\n", ts);
        fflush(stdout);
    } else if (strcmp(cmd, "setpin") == 0) {
        if (!is_authenticated()) {
            printf("Login first: login <pin>\n");
            fflush(stdout);
            return;
        }
        char old_pin[8]="", new_pin[8]="";
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
        char action[16] = "";
        if (sscanf(rest, "%15s", action) != 1) {
            printf("Usage: %s on|off|auto|status\n", cmd);
            fflush(stdout);
            return;
        }
        for (int i = 0; action[i] != '\0'; i++) if (action[i] >= 'a' && action[i] <= 'z') action[i] = action[i] - 'a' + 'A';
        if (strcmp(action, "STATUS") == 0) {
            xSemaphoreTake(s_room_lock, portMAX_DELAY);
            if (room == 1) printf("Room 1: occupied=%d light=%d mode=%s\n", s_room1_occupied, s_room1_light, s_room1_mode);
            else printf("Room 2: occupied=%d light=%d mode=%s\n", s_room2_occupied, s_room2_light, s_room2_mode);
            xSemaphoreGive(s_room_lock);
        } else if (strcmp(action, "ON") == 0 || strcmp(action, "OFF") == 0 || strcmp(action, "AUTO") == 0) {
            room_set_mode(room, action);
            printf("Room %d set to %s\n", room, action);
        } else {
            printf("Usage: %s on|off|auto|status\n", cmd);
        }
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
            if (c == '\n' && last_cr) { last_cr = false; continue; }
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

void app_main(void)
{
    ESP_LOGI(TAG, "CoCIS starting");

    s_state_lock = xSemaphoreCreateMutex();
    s_sd_lock = xSemaphoreCreateMutex();
    s_room_lock = xSemaphoreCreateMutex();

    nvs_setup();
    config_load();
    gpio_setup();
    sd_setup();
    rfid_bus_setup();

    s_entry_ok = rc522_init(&s_entry_reader, SPI2_HOST, RFID_ENTRY_CS_GPIO, RFID_ENTRY_RST_GPIO) == ESP_OK;
    s_exit_ok = rc522_init(&s_exit_reader, SPI2_HOST, RFID_EXIT_CS_GPIO, RFID_EXIT_RST_GPIO) == ESP_OK;
    if (!s_entry_ok) ESP_LOGE(TAG, "entry reader init failed");
    if (!s_exit_ok) ESP_LOGE(TAG, "exit reader init failed");

    setenv("TZ", "EAT-3", 1);
    tzset();
    // Initialize time to a valid value so mktime doesn't crash
    struct timeval tv_init = {.tv_sec = 1761834600, .tv_usec = 0}; // Oct 30 2026 approx
    settimeofday(&tv_init, NULL);

    if (s_entry_ok) xTaskCreate(rfid_task, "rfid_entry", 4096, &s_entry_reader, 5, NULL);
    if (s_exit_ok) xTaskCreate(rfid_task, "rfid_exit", 4096, &s_exit_reader, 5, NULL);
    xTaskCreate(room_task, "rooms", 4096, NULL, 5, NULL);
    xTaskCreate(menu_task, "menu", 8192, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready");
}
