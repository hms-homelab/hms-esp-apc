#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

/* id_slug: optional override; when non-empty the device id becomes "apc_ups_<slug>",
 *          otherwise it is derived from the MAC. device_name: optional HA display name,
 *          falls back to "APC UPS (MAC)" when empty. Pass NULL/"" to use defaults. */
esp_err_t mqtt_init(const char *broker_url, const char *username, const char *password,
                    const char *id_slug, const char *device_name);
esp_err_t mqtt_publish_metric(const char *sensor_name, float value, const char *unit);
esp_err_t mqtt_publish_string(const char *sensor_name, const char *value);
esp_err_t mqtt_publish_discovery(const char *sensor_name, const char *friendly_name, const char *unit, const char *device_class);
bool mqtt_is_connected(void);

#endif // MQTT_MANAGER_H
