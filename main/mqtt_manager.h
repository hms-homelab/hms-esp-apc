#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t mqtt_init(void);
esp_err_t mqtt_publish_metric(const char *sensor_name, float value, const char *unit);
esp_err_t mqtt_publish_string(const char *sensor_name, const char *value);
esp_err_t mqtt_publish_discovery(const char *sensor_name, const char *friendly_name, const char *unit, const char *device_class);
bool mqtt_is_connected(void);

#endif // MQTT_MANAGER_H
