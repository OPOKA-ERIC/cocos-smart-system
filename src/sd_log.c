#include "sd_log.h"
#include "globals.h"
#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"

static const char    *TAG    = "SD";
static sdmmc_card_t  *s_card = NULL;

/* ---------------------------------------------------------------
   SD CARD SETUP
--------------------------------------------------------------- */
void sd_setup(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num     = SD_MOSI_GPIO,
        .miso_io_num     = SD_MISO_GPIO,
        .sclk_io_num     = SD_CLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t err = spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus init failed: %s", esp_err_to_name(err));
        return;
    }

    sdmmc_host_t host     = SDSPI_HOST_DEFAULT();
    host.slot             = SPI3_HOST;
    host.max_freq_khz     = 4000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id               = SPI3_HOST;
    slot.gpio_cs               = SD_CS_GPIO;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot,
                                  &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return;
    }

    g_sd_ok = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
}

/* ---------------------------------------------------------------
   WRITE ONE LINE TO THE LOG FILE
--------------------------------------------------------------- */
static void sd_log_line(const char *line)
{
    if (!g_sd_ok) return;

    if (g_sd_lock == NULL ||
        xSemaphoreTake(g_sd_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return;
    }

    FILE *file = fopen(SD_LOG_FILE, "a");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        if (ftell(file) == 0) {
            fprintf(file, "timestamp,event,uid,name,inside,result\n");
        }
        fprintf(file, "%s\n", line);
        fclose(file);
    }

    xSemaphoreGive(g_sd_lock);
}

/* ---------------------------------------------------------------
   BUILD AND SAVE AN ACCESS LOG ENTRY
--------------------------------------------------------------- */
void log_access(const char *event, const char *uid,
                const char *name, int inside, const char *result)
{
    char timestamp[32];
    fmt_time(timestamp, sizeof(timestamp));

    char line[192];
    snprintf(line, sizeof(line), "%s,%s,%s,%s,%d,%s",
             timestamp, event, uid, name, inside, result);

    ESP_LOGI(TAG, "%s", line);
    sd_log_line(line);
}
