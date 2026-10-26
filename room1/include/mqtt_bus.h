#ifndef MQTT_BUS_H
#define MQTT_BUS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*mqtt_msg_cb_t)(const char *topic, const char *payload, int len);

esp_err_t mqtt_bus_start(const char *host, uint16_t port,
                         const char *client_id,
                         const char *will_topic, const char *will_msg,
                         mqtt_msg_cb_t cb);
esp_err_t mqtt_bus_subscribe(const char *topic);
int mqtt_bus_publish(const char *topic, const char *payload, int len, bool retain);
bool mqtt_bus_connected(void);
void mqtt_bus_publish_status(const char *status);

#endif
