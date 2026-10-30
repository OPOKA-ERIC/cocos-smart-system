#ifndef RC522_H
#define RC522_H

/* -------------------------------------------------------------
   RC522 RFID READER - HEADER FILE
   This file only declares the two functions the rest of the
   program needs. The real work is in src/rc522.c.
   ------------------------------------------------------------- */

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

/* -------------------------------------------------------------
   ONE READER
   This struct holds the two things we need to talk to a reader:
   its SPI handle and its two control pins.
   We create one of these for the entry reader and one for exit.
   ------------------------------------------------------------- */
typedef struct {
    spi_device_handle_t spi;   /* SPI handle used to send/receive data */
    gpio_num_t          cs;    /* chip select pin for this reader */
    gpio_num_t          rst;   /* reset pin for this reader */
} rc522_t;

/* Set up one reader on the given SPI bus and pins.
   Returns ESP_OK if the reader was found and is ready. */
esp_err_t rc522_init(rc522_t *dev, spi_host_device_t host,
                     gpio_num_t cs, gpio_num_t rst);

/* Try to read a card UID.
   Returns true and fills uid/uid_len when a card is present.
   Returns false when no card is present. */
bool rc522_read_uid(rc522_t *dev, uint8_t *uid, uint8_t *uid_len);

#endif
