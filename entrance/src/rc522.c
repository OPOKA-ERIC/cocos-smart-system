#include "rc522.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define RC522_REG_COMMAND       0x01
#define RC522_REG_COM_IRQ       0x04
#define RC522_REG_DIV_IRQ       0x05
#define RC522_REG_ERROR         0x06
#define RC522_REG_FIFO_DATA     0x09
#define RC522_REG_FIFO_LEVEL    0x0A
#define RC522_REG_CONTROL       0x0C
#define RC522_REG_BIT_FRAMING   0x0D
#define RC522_REG_MODE          0x11
#define RC522_REG_TX_CONTROL    0x14
#define RC522_REG_TX_ASK        0x15
#define RC522_REG_CRC_RESULT_H  0x21
#define RC522_REG_CRC_RESULT_L  0x22
#define RC522_REG_T_MODE        0x2A
#define RC522_REG_T_PRESCALER   0x2B
#define RC522_REG_T_RELOAD_H    0x2C
#define RC522_REG_T_RELOAD_L    0x2D
#define RC522_REG_VERSION       0x37

#define RC522_CMD_IDLE          0x00
#define RC522_CMD_CALC_CRC      0x03
#define RC522_CMD_TRANSCEIVE    0x0C
#define RC522_CMD_SOFTRESET     0x0F

#define PICC_CMD_REQA           0x26
#define PICC_CMD_SEL_CL1        0x93
#define PICC_CMD_SEL_CL2        0x95
#define PICC_CMD_SEL_CL3        0x97

static const char *TAG = "RC522";

static uint8_t rc522_addr(uint8_t reg, bool write)
{
    uint8_t addr = (uint8_t)((reg << 1) & 0x7E);
    if (!write) {
        addr |= 0x80;
    }
    return addr;
}

static void rc522_write_reg(rc522_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {rc522_addr(reg, true), value};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 8 * sizeof(tx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(dev->spi, &t);
}

static uint8_t rc522_read_reg(rc522_t *dev, uint8_t reg)
{
    uint8_t tx[2] = {rc522_addr(reg, false), 0x00};
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length = 8 * sizeof(tx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    spi_device_transmit(dev->spi, &t);
    return rx[1];
}

static void rc522_antenna_on(rc522_t *dev)
{
    uint8_t v = rc522_read_reg(dev, RC522_REG_TX_CONTROL);
    if ((v & 0x03) != 0x03) {
        rc522_write_reg(dev, RC522_REG_TX_CONTROL, (uint8_t)(v | 0x03));
    }
}

static void rc522_antenna_off(rc522_t *dev)
{
    uint8_t v = rc522_read_reg(dev, RC522_REG_TX_CONTROL);
    rc522_write_reg(dev, RC522_REG_TX_CONTROL, (uint8_t)(v & ~0x03));
}

static void rc522_reset(rc522_t *dev)
{
    gpio_set_level(dev->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(dev->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    rc522_write_reg(dev, RC522_REG_T_MODE, 0x8D);
    rc522_write_reg(dev, RC522_REG_T_PRESCALER, 0x3E);
    rc522_write_reg(dev, RC522_REG_T_RELOAD_L, 30);
    rc522_write_reg(dev, RC522_REG_T_RELOAD_H, 0);
    rc522_write_reg(dev, RC522_REG_TX_ASK, 0x40);
    rc522_write_reg(dev, RC522_REG_MODE, 0x3D);
    rc522_antenna_on(dev);
}

esp_err_t rc522_init(rc522_t *dev, spi_host_device_t host, gpio_num_t cs, gpio_num_t rst)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->cs = cs;
    dev->rst = rst;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(rst, 1);

    spi_device_interface_config_t cfg = {
        .clock_speed_hz = 1000 * 1000,
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = 4,
    };
    err = spi_bus_add_device(host, &cfg, &dev->spi);
    if (err != ESP_OK) {
        return err;
    }

    rc522_reset(dev);
    uint8_t version = rc522_read_reg(dev, RC522_REG_VERSION);
    ESP_LOGI(TAG, "reader CS=%d RST=%d version=0x%02X", (int)cs, (int)rst, version);
    if (version == 0x00 || version == 0xFF) {
        ESP_LOGW(TAG, "unexpected MFRC522 version on CS=%d", (int)cs);
    }
    return ESP_OK;
}

static bool rc522_transceive(rc522_t *dev, const uint8_t *send, uint8_t send_len,
                             uint8_t *back, uint8_t *back_len, uint8_t *valid_bits)
{
    uint8_t framing = (valid_bits != NULL) ? (uint8_t)(*valid_bits & 0x07) : 0;

    rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_IDLE);
    rc522_write_reg(dev, RC522_REG_COM_IRQ, 0x7F);
    rc522_write_reg(dev, RC522_REG_FIFO_LEVEL, 0x80);
    rc522_write_reg(dev, RC522_REG_BIT_FRAMING, framing);

    for (uint8_t i = 0; i < send_len; i++) {
        rc522_write_reg(dev, RC522_REG_FIFO_DATA, send[i]);
    }

    rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_TRANSCEIVE);
    rc522_write_reg(dev, RC522_REG_BIT_FRAMING, (uint8_t)(framing | 0x80));

    uint8_t irq = 0;
    for (int i = 0; i < 100; i++) {
        irq = rc522_read_reg(dev, RC522_REG_COM_IRQ);
        if (irq & 0x30) {
            break;
        }
        if (irq & 0x01) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if ((irq & 0x30) == 0) {
        return false;
    }

    uint8_t err = rc522_read_reg(dev, RC522_REG_ERROR);
    if (err & 0x1B) {
        return false;
    }

    uint8_t n = rc522_read_reg(dev, RC522_REG_FIFO_LEVEL);
    if (n > *back_len) {
        n = *back_len;
    }
    for (uint8_t i = 0; i < n; i++) {
        back[i] = rc522_read_reg(dev, RC522_REG_FIFO_DATA);
    }
    *back_len = n;
    if (valid_bits != NULL) {
        *valid_bits = rc522_read_reg(dev, RC522_REG_CONTROL) & 0x07;
    }
    return true;
}

static uint16_t rc522_crc(rc522_t *dev, const uint8_t *data, uint8_t len)
{
    rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_IDLE);
    rc522_write_reg(dev, RC522_REG_DIV_IRQ, 0x04);
    rc522_write_reg(dev, RC522_REG_FIFO_LEVEL, 0x80);
    for (uint8_t i = 0; i < len; i++) {
        rc522_write_reg(dev, RC522_REG_FIFO_DATA, data[i]);
    }
    rc522_write_reg(dev, RC522_REG_COMMAND, RC522_CMD_CALC_CRC);

    for (int i = 0; i < 100; i++) {
        if (rc522_read_reg(dev, RC522_REG_DIV_IRQ) & 0x04) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    uint8_t lo = rc522_read_reg(dev, RC522_REG_CRC_RESULT_L);
    uint8_t hi = rc522_read_reg(dev, RC522_REG_CRC_RESULT_H);
    return (uint16_t)(((uint16_t)hi << 8) | lo);
}

static bool rc522_select(rc522_t *dev, uint8_t sel, const uint8_t *part)
{
    uint8_t frame[9];
    frame[0] = sel;
    frame[1] = 0x70;
    memcpy(frame + 2, part, 4);
    frame[6] = part[0] ^ part[1] ^ part[2] ^ part[3];
    uint16_t crc = rc522_crc(dev, frame, 7);
    frame[7] = (uint8_t)(crc & 0xFF);
    frame[8] = (uint8_t)(crc >> 8);

    uint8_t resp[4];
    uint8_t resp_len = sizeof(resp);
    if (!rc522_transceive(dev, frame, sizeof(frame), resp, &resp_len, NULL)) {
        return false;
    }
    return resp_len >= 1;
}

bool rc522_read_uid(rc522_t *dev, uint8_t *uid, uint8_t *uid_len)
{
    if (dev == NULL || uid == NULL || uid_len == NULL) {
        return false;
    }
    uint8_t cmd = PICC_CMD_REQA;
    uint8_t atqa[2];
    uint8_t atqa_len = sizeof(atqa);
    uint8_t valid = 7;
    if (!rc522_transceive(dev, &cmd, 1, atqa, &atqa_len, &valid) || atqa_len != 2) {
        return false;
    }

    uint8_t buf[10];
    int pos = 0;
    uint8_t sel = PICC_CMD_SEL_CL1;
    bool ok = false;

    for (int level = 0; level < 3; level++) {
        uint8_t frame[2] = {sel, 0x20};
        uint8_t resp[5];
        uint8_t resp_len = sizeof(resp);
        uint8_t v = 0;
        if (!rc522_transceive(dev, frame, 2, resp, &resp_len, &v) || resp_len != 5) {
            goto done;
        }
        if ((resp[0] ^ resp[1] ^ resp[2] ^ resp[3]) != resp[4]) {
            goto done;
        }

        if (resp[0] == 0x88) {
            memcpy(buf + pos, resp + 1, 3);
            pos += 3;
            if (!rc522_select(dev, sel, resp)) {
                goto done;
            }
            sel = (level == 0) ? PICC_CMD_SEL_CL2 : PICC_CMD_SEL_CL3;
        } else {
            memcpy(buf + pos, resp, 4);
            pos += 4;
            if (!rc522_select(dev, sel, resp)) {
                goto done;
            }
            break;
        }
    }

    if (pos >= 4) {
        memcpy(uid, buf, pos);
        *uid_len = (uint8_t)pos;
        ok = true;
    }

done:
    rc522_antenna_off(dev);
    vTaskDelay(pdMS_TO_TICKS(10));
    rc522_antenna_on(dev);
    return ok;
}
