#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "config.h"

static const char *TAG = "ENTRANCE";

static sdmmc_card_t *card;
static const char mount_point[] = "/sdcard";
static bool sd_mounted = false;

// People
static person_record_t people[MAX_PEOPLE];
static int people_count = 0;

typedef struct {
    int person_id;
    bool inside;
} person_record_t;

// Config
static int access_start_hour = ACCESS_START_HOUR_DEFAULT;
static int access_end_hour = ACCESS_END_HOUR_DEFAULT;
static int demo_hour = 10;

// Room sync
static bool room1_occupied = false;
static bool room2_occupied = false;

// WiFi/MQTT
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_connected = false;

// SD
static void log_event(const char *event)
{
    if (!sd_mounted) return;
    char path[64];
    snprintf(path, sizeof(path), "%s/entrance_log.txt", mount_point);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", event);
        fclose(f);
    }
}

// NVS
static void load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READONLY, &handle) == ESP_OK) {
        int32_t s = 0, e = 0;
        if (nvs_get_i32(handle, "start_h", &s) == ESP_OK) access_start_hour = (int)s;
        if (nvs_get_i32(handle, "end_h", &e) == ESP_OK) access_end_hour = (int)e;
        nvs_close(handle);
    }
}

static void save_config(int start_h, int end_h)
{
    nvs_handle_t handle;
    if (nvs_open("storage", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "start_h", start_h);
        nvs_set_i32(handle, "end_h", end_h);
        nvs_commit(handle);
        nvs_close(handle);
        access_start_hour = start_h;
        access_end_hour = end_h;
    }
}

// SD init
static void init_sd(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {.format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024};
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {.mosi_io_num = SD_MOSI_GPIO, .miso_io_num = SD_MISO_GPIO, .sclk_io_num = SD_CLK_GPIO, .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4000};
    if (spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA) != ESP_OK) return;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_GPIO;
    slot_config.host_id = host.slot;
    if (esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card) == ESP_OK) {
        sd_mounted = true;
        ESP_LOGI(TAG, "SD mounted");
    }
}

// WiFi
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) esp_wifi_connect();
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_connected = true;
        esp_mqtt_client_subscribe(mqtt_client, "cocis/room1/status", 1);
        esp_mqtt_client_subscribe(mqtt_client, "cocis/room2/status", 1);
        ESP_LOGI(TAG, "MQTT connected, subscribed");
    } else if (event_id == MQTT_EVENT_DATA) {
        char topic[64];
        snprintf(topic, event->topic_len + 1, "%s", event->topic);
        if (event->data_len > 0) {
            char *data = malloc(event->data_len + 1);
            memcpy(data, event->data, event->data_len);
            data[event->data_len] = 0;
            cJSON *root = cJSON_Parse(data);
            if (root) {
                cJSON *occ = cJSON_GetObjectItem(root, "occupied");
                if (cJSON_IsBool(occ)) {
                    if (strstr(topic, "room1")) room1_occupied = cJSON_IsTrue(occ);
                    if (strstr(topic, "room2")) room2_occupied = cJSON_IsTrue(occ);
                    ESP_LOGI(TAG, "Room update from %s: occupied=%d", topic, cJSON_IsTrue(occ));
                }
                cJSON_Delete(root);
            }
            free(data);
        }
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {.broker.address.uri = MQTT_BROKER_URI, .credentials.client_id = MQTT_CLIENT_ID};
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
    wifi_config_t wifi_config = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASS}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Access
static bool access_schedule_open(void)
{
    return demo_hour >= access_start_hour && demo_hour < access_end_hour;
}
static int find_person(int person_id)
{
    for (int i = 0; i < people_count; i++) if (people[i].person_id == person_id) return i;
    return -1;
}
static void process_access(int person_id, bool entry)
{
    if (!access_schedule_open()) {
        printf("ENTRANCE: Schedule CLOSED (%02d:00-%02d:00) - DENIED\n", access_start_hour, access_end_hour);
        char log[80]; snprintf(log, sizeof(log), "DENIED person=%d reason=schedule", person_id); log_event(log);
        return;
    }
    int index = find_person(person_id);
    if (entry) {
        if (index >= 0 && people[index].inside) {
            printf("ENTRANCE: Person %d already inside\n", person_id);
            char log[80]; snprintf(log, sizeof(log), "ALREADY_INSIDE person=%d", person_id); log_event(log);
            return;
        }
        if (index < 0) {
            if (people_count >= MAX_PEOPLE) { printf("ENTRANCE: Capacity reached\n"); return; }
            index = people_count++; people[index].person_id = person_id;
        }
        people[index].inside = true;
        gpio_set_level(DOOR_LOCK_GPIO, 1);
        printf("ENTRANCE: ACCESS GRANTED - Person %d ENTERED\n", person_id);
        char log[80]; snprintf(log, sizeof(log), "GRANTED_ENTER person=%d", person_id); log_event(log);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(DOOR_LOCK_GPIO, 0);
    } else {
        if (index < 0 || !people[index].inside) {
            printf("ENTRPLACE: EXIT DENIED - Person %d not inside\n", person_id);
            char log[80]; snprintf(log, sizeof(log), "DENIED_EXIT person=%d", person_id); log_event(log);
            return;
        }
        people[index].inside = false;
        printf("ENTRANCE: Person %d EXITED\n", person_id);
        char log[80]; snprintf(log, sizeof(log), "EXIT person=%d", person_id); log_event(log);
    }
}
static void print_status(void)
{
    int inside = 0;
    printf("\n--- ENTRANCE STATUS ---\n");
    for (int i = 0; i < people_count; i++) if (people[i].inside) { printf("Person %d: INSIDE\n", people[i].person_id); inside++; }
    printf("Inside: %d | Schedule: %02d:00-%02d:00 | Demo hour: %d\n", inside, access_start_hour, access_end_hour, demo_hour);
    printf("Rooms: ROOM1=%s | ROOM2=%s\n", room1_occupied ? "OCCUPIED" : "UNOCCUPIED", room2_occupied ? "OCCUPIED" : "UNOCCUPIED");
    printf("-----------------------\n\n");
}

// Menu
static void menu_task(void *pvParameters)
{
    char cmd[64];
    while (1) {
        printf("\n=== MENU ===\n1) Status\n2) Enter (ID)\n3) Exit (ID)\n4) Schedule (auth)\n5) Logs\n6) Demo hour\n7) Room sync\nEnter: ");
        fflush(stdout);
        if (fgets(cmd, sizeof(cmd), stdin)) {
            cmd[strcspn(cmd, "\r\n")] = 0;
            if (strcmp(cmd, "1") == 0) print_status();
            else if (strcmp(cmd, "2") == 0) { printf("ID: "); fflush(stdout); if (fgets(cmd, sizeof(cmd), stdin)) { cmd[strcspn(cmd, "\r\n")] = 0; process_access(atoi(cmd), true); } }
            else if (strcmp(cmd, "3") == 0) { printf("ID: "); fflush(stdout); if (fgets(cmd, sizeof(cmd), stdin)) { cmd[strcspn(cmd, "\r\n")] = 0; process_access(atoi(cmd), false); } }
            else if (strcmp(cmd, "4") == 0) { printf("PIN: "); fflush(stdout); if (fgets(cmd, sizeof(cmd), stdin)) { cmd[strcspn(cmd, "\r\n")] = 0; if (strcmp(cmd, ADMIN_PIN) == 0) { printf("Start(0-23): "); fflush(stdout); char s[16]; if (fgets(s,sizeof(s),stdin)){s[strcspn(s,"\r\n")]=0; printf("End(0-23): "); fflush(stdout); char e[16]; if(fgets(e,sizeof(e),stdin)){e[strcspn(e,"\r\n")]=0; save_config(atoi(s),atoi(e)); printf("Updated\n");}} } else printf("Bad PIN\n"); } }
            else if (strcmp(cmd, "5") == 0) {
                if (sd_mounted) { FILE *f=fopen("/sdcard/entrance_log.txt","r"); if(f){char l[128]; while(fgets(l,sizeof(l),f)) printf("%s",l); fclose(f);} else printf("No logs\n"); } else printf("SD missing\n");
            }
            else if (strcmp(cmd, "6") == 0) { printf("Hour(0-23): "); fflush(stdout); if (fgets(cmd,sizeof(cmd),stdin)){cmd[strcspn(cmd,"\r\n")]=0; demo_hour=atoi(cmd); printf("Set %d\n",demo_hour);} }
            else if (strcmp(cmd, "7") == 0) { printf("ROOM1:%s ROOM2:%s\n", room1_occupied?"OCCUPIED":"UNOCCUPIED", room2_occupied?"OCCUPIED":"UNOCCUPIED"); }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init(); }
    ESP_ERROR_CHECK(ret);

    gpio_config_t door = {.pin_bit_mask = (1ULL<<DOOR_LOCK_GPIO), .mode = GPIO_MODE_OUTPUT, .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&door)); gpio_set_level(DOOR_LOCK_GPIO,0);

    load_config(); init_sd(); wifi_init();
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    mqtt_app_start();

    printf("\n=== CO-CIS ENTRANCE ===\nSchedule:%02d:%02d-%02d:%02d\n",0,access_start_hour,0,access_end_hour);
    process_access(1001,true); process_access(1002,true); process_access(1001,true); print_status(); process_access(1001,false); print_status();
    xTaskCreate(menu_task, "menu", 4096, NULL, 5, NULL);
}
