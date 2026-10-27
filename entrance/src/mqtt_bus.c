#include "mqtt_bus.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"

#define MQTT_MAX_SUBS     8
#define MQTT_TOPIC_MAX    96
#define MQTT_HOST_MAX     64
#define MQTT_CLIENT_MAX   48
#define MQTT_PAYLOAD_MAX  512

static const char *TAG = "MQTT";

static int s_sock = -1;
static bool s_started = false;
static bool s_connected = false;
static bool s_warned = false;
static SemaphoreHandle_t s_tx_lock = NULL;
static SemaphoreHandle_t s_state_lock = NULL;
static mqtt_msg_cb_t s_cb = NULL;

static char s_host[MQTT_HOST_MAX];
static uint16_t s_port;
static char s_client_id[MQTT_CLIENT_MAX];
static char s_status_topic[MQTT_TOPIC_MAX];
static char s_will_msg[32];

static char s_subs[MQTT_MAX_SUBS][MQTT_TOPIC_MAX];
static int s_sub_count = 0;
static uint16_t s_packet_id = 1;

static int send_all(int sock, const uint8_t *data, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += n;
    }
    return 0;
}

static int recv_exact(int sock, uint8_t *data, int len)
{
    int got = 0;
    while (got < len) {
        int n = recv(sock, data + got, len - got, 0);
        if (n <= 0) {
            return -1;
        }
        got += n;
    }
    return 0;
}

static int encode_remaining(uint8_t *buf, int len)
{
    int i = 0;
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len > 0) {
            b |= 0x80;
        }
        buf[i++] = b;
    } while (len > 0);
    return i;
}

static int read_remaining(int sock)
{
    int value = 0;
    int multiplier = 1;
    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (recv_exact(sock, &b, 1) != 0) {
            return -1;
        }
        value += (b & 0x7F) * multiplier;
        if ((b & 0x80) == 0) {
            return value;
        }
        multiplier *= 128;
    }
    return -1;
}

static int tx_send(const uint8_t *data, int len)
{
    if (s_tx_lock == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return -1;
    }
    int rc = -1;
    if (s_sock >= 0) {
        rc = send_all(s_sock, data, len);
    }
    xSemaphoreGive(s_tx_lock);
    return rc;
}

static int send_publish_locked(const char *topic, const char *payload, int len, bool retain)
{
    int tlen = (int)strlen(topic);
    if (tlen <= 0 || tlen > 65535 || len < 0) {
        return -1;
    }
    int rem = 2 + tlen + len;
    uint8_t hdr[6];
    int h = 0;
    hdr[h++] = (uint8_t)(0x30 | (retain ? 0x01 : 0x00));
    h += encode_remaining(hdr + h, rem);

    uint8_t tbuf[2] = {(uint8_t)(tlen >> 8), (uint8_t)(tlen & 0xFF)};
    if (send_all(s_sock, hdr, h) != 0) {
        return -1;
    }
    if (send_all(s_sock, tbuf, 2) != 0) {
        return -1;
    }
    if (send_all(s_sock, (const uint8_t *)topic, tlen) != 0) {
        return -1;
    }
    if (len > 0 && send_all(s_sock, (const uint8_t *)payload, len) != 0) {
        return -1;
    }
    return 0;
}

static int send_subscribe_locked(const char *topic)
{
    int tlen = (int)strlen(topic);
    if (tlen <= 0 || tlen > 65535) {
        return -1;
    }
    uint16_t pid = s_packet_id++;
    if (s_packet_id == 0) {
        s_packet_id = 1;
    }
    int rem = 2 + 2 + tlen + 1;
    uint8_t pkt[160];
    int p = 0;
    pkt[p++] = 0x82;
    p += encode_remaining(pkt + p, rem);
    pkt[p++] = (uint8_t)(pid >> 8);
    pkt[p++] = (uint8_t)(pid & 0xFF);
    pkt[p++] = (uint8_t)(tlen >> 8);
    pkt[p++] = (uint8_t)(tlen & 0xFF);
    memcpy(pkt + p, topic, tlen);
    p += tlen;
    pkt[p++] = 0x00;
    return send_all(s_sock, pkt, p);
}

static int mqtt_connect(void)
{
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[8];
    snprintf(portbuf, sizeof(portbuf), "%u", s_port);

    int gai = getaddrinfo(s_host, portbuf, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGW(TAG, "DNS failed for %s: gai=%d", s_host, gai);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    char ip[16] = "?";
    inet_ntoa_r(addr->sin_addr, ip, sizeof(ip));

    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect to %s:%u (%s) failed: errno=%d", s_host, s_port, ip, errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    uint8_t body[220];
    int b = 0;
    int cidlen = (int)strlen(s_client_id);
    int wtlen = (int)strlen(s_status_topic);
    int wmlen = (int)strlen(s_will_msg);

    body[b++] = 0x00;
    body[b++] = 'M';
    body[b++] = 'Q';
    body[b++] = 'T';
    body[b++] = 0x04;
    uint8_t flags = 0x02; // clean session, no will
    body[b++] = flags;
    body[b++] = 0x00;
    body[b++] = 0x3C;
    body[b++] = (uint8_t)(cidlen >> 8);
    body[b++] = (uint8_t)(cidlen & 0xFF);
    memcpy(body + b, s_client_id, cidlen);
    b += cidlen;

    uint8_t pkt[256];
    int p = 0;
    pkt[p++] = 0x10;
    p += encode_remaining(pkt + p, b);
    if (p + b > (int)sizeof(pkt)) {
        close(sock);
        return -1;
    }
    memcpy(pkt + p, body, b);
    p += b;

    if (send_all(sock, pkt, p) != 0) {
        close(sock);
        return -1;
    }

    fd_set rfds;
    struct timeval tv2 = {.tv_sec = 3, .tv_usec = 0};
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    if (select(sock + 1, &rfds, NULL, NULL, &tv2) <= 0) {
        ESP_LOGW(TAG, "CONNACK timeout");
        close(sock);
        return -1;
    }

    uint8_t ack[4] = {0};
    int nr = recv(sock, ack, 4, 0);
    if (nr != 4 || ack[0] != 0x20 || ack[3] != 0x00) {
        ESP_LOGW(TAG, "CONNACK failed: got=%d hdr=%02X code=%u", nr, ack[0], ack[3]);
        close(sock);
        return -1;
    }

    struct timeval rtv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    s_sock = sock;
    ESP_LOGI(TAG, "connected to %s:%u (%s)", s_host, s_port, ip);
    return 0;
}

static void dispatch_publish(uint8_t hdr, const uint8_t *buf, int rem)
{
    int qos = (hdr >> 1) & 0x03;
    if (rem < 2) {
        return;
    }
    int tlen = (buf[0] << 8) | buf[1];
    if (tlen + 2 > rem) {
        return;
    }
    char topic[MQTT_TOPIC_MAX];
    int tcopy = tlen < (int)sizeof(topic) - 1 ? tlen : (int)sizeof(topic) - 1;
    memcpy(topic, buf + 2, tcopy);
    topic[tcopy] = '\0';

    const uint8_t *payload = buf + 2 + tlen;
    int plen = rem - 2 - tlen;
    if (qos > 0 && plen >= 2) {
        uint8_t pid_hi = payload[0];
        uint8_t pid_lo = payload[1];
        payload += 2;
        plen -= 2;
        uint8_t puback[4] = {0x40, 0x02, pid_hi, pid_lo};
        tx_send(puback, sizeof(puback));
    }

    char pbuf[MQTT_PAYLOAD_MAX];
    int pcopy = plen < (int)sizeof(pbuf) - 1 ? plen : (int)sizeof(pbuf) - 1;
    memcpy(pbuf, payload, pcopy);
    pbuf[pcopy] = '\0';

    if (s_cb != NULL) {
        s_cb(topic, pbuf, pcopy);
    }
}

static void read_loop(void)
{
    uint8_t buf[MQTT_PAYLOAD_MAX + MQTT_TOPIC_MAX];
    TickType_t last_ping = xTaskGetTickCount();

    while (true) {
        if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
            bool conn = s_connected;
            xSemaphoreGive(s_state_lock);
            if (!conn) {
                break;
            }
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_ping) > pdMS_TO_TICKS(30000)) {
            uint8_t ping[2] = {0xC0, 0x00};
            if (tx_send(ping, sizeof(ping)) != 0) {
                break;
            }
            last_ping = now;
        }

        uint8_t hdr;
        int n = recv(s_sock, &hdr, 1, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }

        int rem = read_remaining(s_sock);
        if (rem < 0) {
            break;
        }
        if (rem > (int)sizeof(buf)) {
            uint8_t tmp[64];
            int left = rem;
            bool bad = false;
            while (left > 0) {
                int chunk = left > (int)sizeof(tmp) ? (int)sizeof(tmp) : left;
                if (recv_exact(s_sock, tmp, chunk) != 0) {
                    bad = true;
                    break;
                }
                left -= chunk;
            }
            if (bad) {
                break;
            }
            continue;
        }
        if (rem > 0 && recv_exact(s_sock, buf, rem) != 0) {
            break;
        }

        uint8_t type = hdr >> 4;
        if (type == 3) {
            dispatch_publish(hdr, buf, rem);
        }
    }
}

static void mqtt_task(void *arg)
{
    (void)arg;
    while (true) {
        if (mqtt_connect() == 0) {
            s_warned = false;
            if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
                s_connected = true;
                xSemaphoreGive(s_state_lock);
            }
            mqtt_bus_publish_status("online");

            char subs[MQTT_MAX_SUBS][MQTT_TOPIC_MAX];
            int nsubs = 0;
            if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
                nsubs = s_sub_count;
                for (int i = 0; i < nsubs; i++) {
                    strlcpy(subs[i], s_subs[i], sizeof(subs[i]));
                }
                xSemaphoreGive(s_state_lock);
            }
            if (xSemaphoreTake(s_tx_lock, portMAX_DELAY) == pdTRUE) {
                for (int i = 0; i < nsubs; i++) {
                    if (send_subscribe_locked(subs[i]) != 0) {
                        break;
                    }
                }
                xSemaphoreGive(s_tx_lock);
            }

            read_loop();

            if (xSemaphoreTake(s_state_lock, portMAX_DELAY) == pdTRUE) {
                s_connected = false;
                xSemaphoreGive(s_state_lock);
            }
            if (!s_warned) {
                ESP_LOGW(TAG, "MQTT disconnected; retrying quietly in background");
                s_warned = true;
            }
        } else if (!s_warned) {
            ESP_LOGW(TAG, "MQTT broker unavailable; retrying every 10s (RFID/SD still work)");
            s_warned = true;
        }

        if (s_tx_lock != NULL && xSemaphoreTake(s_tx_lock, portMAX_DELAY) == pdTRUE) {
            if (s_sock >= 0) {
                close(s_sock);
                s_sock = -1;
            }
            xSemaphoreGive(s_tx_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

esp_err_t mqtt_bus_start(const char *host, uint16_t port,
                         const char *client_id,
                         const char *will_topic, const char *will_msg,
                         mqtt_msg_cb_t cb)
{
    if (host == NULL || client_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_OK;
    }
    strlcpy(s_host, host, sizeof(s_host));
    s_port = port;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_client_id, sizeof(s_client_id), "%s-%02X%02X", client_id, mac[4], mac[5]);
    } else {
        snprintf(s_client_id, sizeof(s_client_id), "%s-%04X", client_id,
                 (unsigned)(esp_random() & 0xFFFF));
    }
    strlcpy(s_status_topic, will_topic != NULL ? will_topic : "", sizeof(s_status_topic));
    strlcpy(s_will_msg, will_msg != NULL ? will_msg : "offline", sizeof(s_will_msg));
    s_cb = cb;

    s_tx_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL || s_state_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(mqtt_task, "mqtt_bus", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t mqtt_bus_subscribe(const char *topic)
{
    if (topic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int tlen = (int)strlen(topic);
    if (tlen <= 0 || tlen >= MQTT_TOPIC_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    bool send_now = false;
    if (s_state_lock == NULL || xSemaphoreTake(s_state_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    bool found = false;
    for (int i = 0; i < s_sub_count; i++) {
        if (strcmp(s_subs[i], topic) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        if (s_sub_count >= MQTT_MAX_SUBS) {
            xSemaphoreGive(s_state_lock);
            return ESP_ERR_NO_MEM;
        }
        strlcpy(s_subs[s_sub_count++], topic, MQTT_TOPIC_MAX);
        send_now = s_connected;
    }
    xSemaphoreGive(s_state_lock);

    if (send_now && s_tx_lock != NULL) {
        if (xSemaphoreTake(s_tx_lock, portMAX_DELAY) == pdTRUE) {
            if (s_sock >= 0) {
                send_subscribe_locked(topic);
            }
            xSemaphoreGive(s_tx_lock);
        }
    }
    return ESP_OK;
}

int mqtt_bus_publish(const char *topic, const char *payload, int len, bool retain)
{
    if (topic == NULL || payload == NULL || len < 0) {
        return -1;
    }
    if (!mqtt_bus_connected()) {
        return -1;
    }
    if (s_tx_lock == NULL || xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return -1;
    }
    int rc = -1;
    if (s_connected && s_sock >= 0) {
        rc = send_publish_locked(topic, payload, len, retain);
        if (rc != 0) {
            s_connected = false;
            shutdown(s_sock, SHUT_RDWR);
        }
    }
    xSemaphoreGive(s_tx_lock);
    return rc;
}

bool mqtt_bus_connected(void)
{
    if (s_state_lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_state_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    bool conn = s_connected;
    xSemaphoreGive(s_state_lock);
    return conn;
}

void mqtt_bus_publish_status(const char *status)
{
    if (s_status_topic[0] == '\0' || status == NULL) {
        return;
    }
    mqtt_bus_publish(s_status_topic, status, (int)strlen(status), true);
}
