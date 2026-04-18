#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char mqtt_url[128];
    char mqtt_user[64];
    char mqtt_pass[64];
    uint32_t publish_interval_ms;
} app_config_t;

esp_err_t config_load(app_config_t *config);
esp_err_t http_server_start(app_config_t *config);

#endif // HTTP_SERVER_H
