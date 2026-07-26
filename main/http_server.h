#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char mqtt_url[128];
    char mqtt_user[64];
    char mqtt_pass[64];
    uint32_t publish_interval_ms;
    /* Optional identity overrides (blank = MAC-derived defaults) */
    char device_slug[32];   /* becomes device id "apc_ups_<slug>"; blank => MAC */
    char device_name[64];   /* HA device display name; blank => "APC UPS (MAC)" */
    /* false = nothing was ever saved to NVS, so we boot straight into the
     * SoftAP config portal instead of trying the Kconfig-compiled SSID. */
    bool provisioned;
} app_config_t;

esp_err_t config_load(app_config_t *config);
esp_err_t http_server_start(app_config_t *config);

#endif // HTTP_SERVER_H
