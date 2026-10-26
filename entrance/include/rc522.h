#ifndef RC522_H
#define RC522_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t cs;
    gpio_num_t rst;
} rc522_t;

esp_err_t rc522_init(rc522_t *dev, spi_host_device_t host, gpio_num_t cs, gpio_num_t rst);
bool rc522_read_uid(rc522_t *dev, uint8_t *uid, uint8_t *uid_len);

#endif
