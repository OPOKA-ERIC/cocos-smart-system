/* =============================================================
   RC522 RFID READER DRIVER
   This file knows how to talk to the MFRC522 RFID chip. The chip
   is connected to the ESP32 over SPI. The rest of the program only
   calls rc522_init() and rc522_read_uid(); everything else here is
   the low-level plumbing.

   HOW TO READ THIS FILE
   The chip is controlled by reading and writing "registers".
   Think of a register as a numbered box inside the chip. We put a
   value into a box to tell the chip what to do, and we read a box
   to find out what happened. The datasheet gives each box a number.
   ============================================================= */

#include "rc522.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "RC522";

/* -------------------------------------------------------------
   SECTION 1: REGISTER NUMBERS
   These are the internal box numbers of the MFRC522 chip.
   ------------------------------------------------------------- */
#define RC522_REG_COMMAND      0x01  /* tells the chip what to do */
#define RC522_REG_COM_IRQ      0x04  /* interrupt flags: tells us when done */
#define RC522_REG_DIV_IRQ      0x05  /* extra interrupt flags */
#define RC522_REG_ERROR        0x06  /* error flags */
#define RC522_REG_FIFO_DATA    0x09  /* read/write data bytes here */
#define RC522_REG_FIFO_LEVEL   0x0A  /* how many bytes are in the FIFO buffer */
#define RC522_REG_CONTROL      0x0C  /* misc control bits */
#define RC522_REG_BIT_FRAMING  0x0D  /* controls how many bits to send */
#define RC522_REG_MODE         0x11  /* general mode settings */
#define RC522_REG_TX_CONTROL   0x14  /* controls the antenna transmit pins */
#define RC522_REG_TX_ASK       0x15  /* modulation settings */
#define RC522_REG_CRC_RESULT_H 0x21  /* CRC result high byte */
#define RC522_REG_CRC_RESULT_L 0x22  /* CRC result low byte */
#define RC522_REG_T_MODE       0x2A  /* timer mode */
#define RC522_REG_T_PRESCALER  0x2B  /* timer prescaler */
#define RC522_REG_T_RELOAD_H   0x2C  /* timer reload high byte */
#define RC522_REG_T_RELOAD_L   0x2D  /* timer reload low byte */
#define RC522_REG_VERSION      0x37  /* chip version - used to check it works */

/* -------------------------------------------------------------
   SECTION 2: CHIP COMMANDS
   We write one of these values into RC522_REG_COMMAND to make the
   chip do something.
   ------------------------------------------------------------- */
#define RC522_CMD_IDLE         0x00  /* stop whatever is running */
#define RC522_CMD_CALC_CRC     0x03  /* calculate a CRC checksum */
#define RC522_CMD_TRANSCEIVE   0x0C  /* send data and receive a response */
#define RC522_CMD_SOFTRESET    0x0F  /* reset the chip */

/* -------------------------------------------------------------
   SECTION 3: CARD COMMANDS
   These are commands we send to the RFID card itself (not to the
   reader chip).
   ------------------------------------------------------------- */
#define PICC_CMD_REQA    0x26  /* ask if any card is nearby */
#define PICC_CMD_SEL_CL1 0x93  /* select card - cascade level 1 */
#define PICC_CMD_SEL_CL2 0x95  /* select card - cascade level 2 (7-byte UIDs) */
#define PICC_CMD_SEL_CL3 0x97  /* select card - cascade level 3 */

/* -------------------------------------------------------------
   SECTION 4: SPI ADDRESS HELPER
   The MFRC522 expects the register number in a special format:
     - bit 7 is 1 for a read, 0 for a write
     - bits 6-1 are the register number shifted left by one
   ------------------------------------------------------------- */
static uint8_t rc522_addr(uint8_t reg, bool write)
{
    uint8_t address = (uint8_t)((reg << 1) & 0x7E);

    if (!write) {
        address |= 0x80;  /* set bit 7 to say "this is a read" */
    }

    return address;
}

/* -------------------------------------------------------------
   SECTION 5: READ / WRITE ONE REGISTER
   These are the smallest building blocks: send one byte to a
   register, or read one byte back.
   ------------------------------------------------------------- */

/* Write one byte into a register. */
static void rc522_write_reg(rc522_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { rc522_addr(reg, true), value };
    uint8_t rx[2] = { 0 };

    spi_transaction_t transaction = {
        .length    = 8 * sizeof(tx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    spi_device_transmit(dev->spi, &transaction);
}

/* Read one byte from a register. */
static uint8_t rc522_read_reg(rc522_t *dev, uint8_t reg)
{
    uint8_t tx[2] = { rc522_addr(reg, false), 0x00 };
    uint8_t rx[2] = { 0 };

    spi_transaction_t transaction = {
        .length    = 8 * sizeof(tx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    spi_device_transmit(dev->spi, &transaction);

    return rx[1];  /* the answer comes back in the second byte */
}

/* -------------------------------------------------------------
   SECTION 6: THE FIFO BUFFER
   The chip has a small internal buffer called the FIFO. We put data
   into it before sending to a card, and read data from it after the
   card replies.
   ------------------------------------------------------------- */

/* Read several bytes from the FIFO. */
static void rc522_read_fifo(rc522_t *dev, uint8_t *buffer, uint8_t length)
{
    if (length == 0) {
        return;
    }

    uint8_t tx[1 + 16] = { 0 };
    uint8_t rx[1 + 16] = { 0 };

    tx[0] = rc522_addr(RC522_REG_FIFO_DATA, false);

    spi_transaction_t transaction = {
        .length    = 8 * (1 + length),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    spi_device_transmit(dev->spi, &transaction);

    memcpy(buffer, rx + 1, length);
}

/* Write several bytes into the FIFO. */
static void rc522_write_fifo(rc522_t *dev, const uint8_t *buffer, uint8_t length)
{
    if (length == 0) {
        return;
    }

    uint8_t tx[1 + 16] = { 0 };
    tx[0] = rc522_addr(RC522_REG_FIFO_DATA, true);
    memcpy(tx + 1, buffer, length);

    spi_transaction_t transaction = {
        .length    = 8 * (1 + length),
        .tx_buffer = tx,
    };

    spi_device_transmit(dev->spi, &transaction);
}

/* -------------------------------------------------------------
   SECTION 7: ANTENNA CONTROL
   The antenna must be on for the reader to detect cards. We turn it
   off and on again after each read to reset the radio field.
   ------------------------------------------------------------- */

static void rc522_antenna_on(rc522_t *dev)
{
    uint8_t value = rc522_read_reg(dev, RC522_REG_TX_CONTROL);

    if ((value & 0x03) != 0x03) {
        rc522_write_reg(dev, RC522_REG_TX_CONTROL, (uint8_t)(value | 0x03));
    }
}

static void rc522_antenna_off(rc522_t *dev)
{
    uint8_t value = rc522_read_reg(dev, RC522_REG_TX_CONTROL);
    rc522_write_reg(dev, RC522_REG_TX_CONTROL, (uint8_t)(value & ~0x03));
}

/* -------------------------------------------------------------
   SECTION 8: CHIP RESET AND STARTUP SETTINGS
   Pull the RST pin low and then high to reset the chip, then write
   the standard startup values.
   ------------------------------------------------------------- */
static void rc522_reset(rc522_t *dev)
{
    /* Hardware reset through the RST pin. */
    gpio_set_level(dev->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(dev->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Software reset command. */
    rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Configure the internal timer (used for card timeouts). */
    rc522_write_reg(dev, RC522_REG_T_MODE,      0x8D);
    rc522_write_reg(dev, RC522_REG_T_PRESCALER, 0x3E);
    rc522_write_reg(dev, RC522_REG_T_RELOAD_L,  30);
    rc522_write_reg(dev, RC522_REG_T_RELOAD_H,  0);

    /* Standard modulation and CRC settings. */
    rc522_write_reg(dev, RC522_REG_TX_ASK, 0x40);
    rc522_write_reg(dev, RC522_REG_MODE,   0x3D);

    rc522_antenna_on(dev);
}

/* -------------------------------------------------------------
   SECTION 9: INITIALISE ONE READER
   Call this once for each reader at startup.
   ------------------------------------------------------------- */
esp_err_t rc522_init(rc522_t *dev, spi_host_device_t host,
                     gpio_num_t cs, gpio_num_t rst)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->cs  = cs;
    dev->rst = rst;

    /* Configure the reset pin as an output. */
    gpio_config_t io_config = {
        .pin_bit_mask  = (1ULL << rst),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_config);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(rst, 1);

    /* Add this reader as a device on the SPI bus. */
    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 1000 * 1000,  /* 1 MHz SPI clock */
        .mode           = 0,
        .spics_io_num   = cs,
        .queue_size     = 4,
    };

    err = spi_bus_add_device(host, &device_config, &dev->spi);
    if (err != ESP_OK) {
        return err;
    }

    /* Reset the chip and apply the startup settings. */
    rc522_reset(dev);

    /* Read the version register to confirm the chip responds. */
    uint8_t version = rc522_read_reg(dev, RC522_REG_VERSION);
    ESP_LOGI(TAG, "reader CS=%d RST=%d version=0x%02X",
             (int)cs, (int)rst, version);

    if (version == 0x00 || version == 0xFF) {
        ESP_LOGW(TAG, "unexpected MFRC522 version on CS=%d", (int)cs);
    }

    return ESP_OK;
}

/* -------------------------------------------------------------
   SECTION 10: SEND DATA TO A CARD AND WAIT FOR A REPLY
   This is the core communication function. It:
     1. clears the FIFO
     2. loads the data we want to send
     3. starts the TRANSCEIVE command
     4. waits for the card to answer
     5. reads the answer back from the FIFO
   ------------------------------------------------------------- */
static bool rc522_transceive(rc522_t *dev,
                             const uint8_t *send, uint8_t send_length,
                             uint8_t *reply, uint8_t *reply_length,
                             uint8_t *valid_bits)
{
    uint8_t framing = 0;
    if (valid_bits != NULL) {
        framing = (uint8_t)(*valid_bits & 0x07);
    }

    /* Clear the FIFO and any old interrupt flags. */
    rc522_write_reg(dev, RC522_REG_COMMAND,    RC522_CMD_IDLE);
    rc522_write_reg(dev, RC522_REG_COM_IRQ,    0x7F);
    rc522_write_reg(dev, RC522_REG_FIFO_LEVEL, 0x80);
    rc522_write_reg(dev, RC522_REG_BIT_FRAMING, framing);

    /* Load the data we want to send into the FIFO. */
    rc522_write_fifo(dev, send, send_length);

    /* Start sending and listening. */
    rc522_write_reg(dev, RC522_REG_COMMAND,     RC522_CMD_TRANSCEIVE);
    rc522_write_reg(dev, RC522_REG_BIT_FRAMING, (uint8_t)(framing | 0x80));

    /* Wait up to 100 ms for the card to reply. */
    uint8_t irq = 0;

    for (int i = 0; i < 100; i++) {
        irq = rc522_read_reg(dev, RC522_REG_COM_IRQ);

        if (irq & 0x30) {
            break;  /* data received or chip idle - we are done */
        }
        if (irq & 0x01) {
            return false;  /* timer expired - no card */
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if ((irq & 0x30) == 0) {
        return false;  /* timed out */
    }

    /* Check the error register. */
    uint8_t error = rc522_read_reg(dev, RC522_REG_ERROR);
    if (error & 0x1B) {
        return false;
    }

    /* Read the reply bytes from the FIFO. */
    uint8_t count = rc522_read_reg(dev, RC522_REG_FIFO_LEVEL);
    if (count > *reply_length) {
        count = *reply_length;
    }

    rc522_read_fifo(dev, reply, count);
    *reply_length = count;

    if (valid_bits != NULL) {
        *valid_bits = rc522_read_reg(dev, RC522_REG_CONTROL) & 0x07;
    }

    return true;
}

/* -------------------------------------------------------------
   SECTION 11: CRC CHECKSUM
   The MFRC522 can calculate a CRC checksum in hardware. We use it
   when selecting a card to prove the UID was received correctly.
   ------------------------------------------------------------- */
static uint16_t rc522_crc(rc522_t *dev, const uint8_t *data, uint8_t length)
{
    rc522_write_reg(dev, RC522_REG_COMMAND,    RC522_CMD_IDLE);
    rc522_write_reg(dev, RC522_REG_DIV_IRQ,    0x04);
    rc522_write_reg(dev, RC522_REG_FIFO_LEVEL, 0x80);
    rc522_write_fifo(dev, data, length);
    rc522_write_reg(dev, RC522_REG_COMMAND,    RC522_CMD_CALC_CRC);

    /* Wait for the CRC calculation to finish. */
    for (int i = 0; i < 100; i++) {
        if (rc522_read_reg(dev, RC522_REG_DIV_IRQ) & 0x04) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint8_t low  = rc522_read_reg(dev, RC522_REG_CRC_RESULT_L);
    uint8_t high = rc522_read_reg(dev, RC522_REG_CRC_RESULT_H);

    return (uint16_t)(((uint16_t)high << 8) | low);
}

/* -------------------------------------------------------------
   SECTION 12: SELECT A CARD
   After anticollision we "select" the card by sending back its full
   UID with a CRC. The card confirms that it was selected.
   ------------------------------------------------------------- */
static bool rc522_select(rc522_t *dev, uint8_t select_command,
                         const uint8_t *uid_part)
{
    uint8_t frame[9];

    frame[0] = select_command;
    frame[1] = 0x70;  /* NVB: 7 bytes follow */
    memcpy(frame + 2, uid_part, 4);

    /* BCC is a simple XOR checksum of the four UID bytes. */
    frame[6] = uid_part[0] ^ uid_part[1] ^ uid_part[2] ^ uid_part[3];

    uint16_t crc = rc522_crc(dev, frame, 7);
    frame[7] = (uint8_t)(crc & 0xFF);
    frame[8] = (uint8_t)(crc >> 8);

    uint8_t reply[4];
    uint8_t reply_length = sizeof(reply);

    if (!rc522_transceive(dev, frame, sizeof(frame),
                          reply, &reply_length, NULL)) {
        return false;
    }

    return reply_length >= 1;
}

/* -------------------------------------------------------------
   SECTION 13: READ A CARD UID (the main public function)
   This is what the rest of the program calls. The steps are:
     1. Ask if any card is nearby (REQA).
     2. Read the UID, one "cascade level" at a time.
        A normal card has a 4-byte UID. Some cards have a 7-byte
        UID, which needs two cascade levels.
     3. Select the card to confirm the UID.
   Returns true and fills uid/uid_len when a card is found.
   ------------------------------------------------------------- */
bool rc522_read_uid(rc522_t *dev, uint8_t *uid, uint8_t *uid_len)
{
    if (dev == NULL || uid == NULL || uid_len == NULL) {
        return false;
    }

    /* --- Step 1: ask if any card is nearby --- */
    uint8_t request = PICC_CMD_REQA;
    uint8_t atqa[2] = { 0 };
    uint8_t atqa_length = sizeof(atqa);
    uint8_t valid_bits  = 7;

    if (!rc522_transceive(dev, &request, 1,
                          atqa, &atqa_length, &valid_bits)) {
        return false;  /* no card present */
    }

    if (atqa_length != 2) {
        return false;
    }

    /* --- Step 2: anticollision loop --- */
    uint8_t collected[10];   /* the UID bytes we gather */
    int     collected_len = 0;
    uint8_t select_command = PICC_CMD_SEL_CL1;
    bool    success = false;

    for (int level = 0; level < 3; level++) {
        /* Send the anticollision command. */
        uint8_t frame[2] = { select_command, 0x20 };
        uint8_t reply[5] = { 0 };
        uint8_t reply_length = sizeof(reply);
        uint8_t reply_valid_bits = 0;

        if (!rc522_transceive(dev, frame, 2,
                              reply, &reply_length, &reply_valid_bits)) {
            goto done;
        }

        if (reply_length != 5) {
            goto done;
        }

        /* Verify the BCC (XOR checksum) byte. */
        if ((reply[0] ^ reply[1] ^ reply[2] ^ reply[3]) != reply[4]) {
            goto done;
        }

        if (reply[0] == 0x88) {
            /* Cascade tag: the UID continues in the next level. */
            memcpy(collected + collected_len, reply + 1, 3);
            collected_len += 3;

            if (!rc522_select(dev, select_command, reply)) {
                goto done;
            }

            select_command = (level == 0) ? PICC_CMD_SEL_CL2
                                          : PICC_CMD_SEL_CL3;
        } else {
            /* Last cascade level: copy the final four UID bytes. */
            memcpy(collected + collected_len, reply, 4);
            collected_len += 4;

            if (!rc522_select(dev, select_command, reply)) {
                goto done;
            }
            break;
        }
    }

    /* --- Step 3: hand the complete UID back to the caller --- */
    if (collected_len >= 4) {
        memcpy(uid, collected, collected_len);
        *uid_len = (uint8_t)collected_len;
        success = true;
    }

done:
    /* Briefly reset the radio field so the next read starts fresh. */
    rc522_antenna_off(dev);
    vTaskDelay(pdMS_TO_TICKS(10));
    rc522_antenna_on(dev);

    return success;
}
