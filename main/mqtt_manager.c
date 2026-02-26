#include "mqtt_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mqtt_manager";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;

// Device ID based on MAC address (e.g., "apc_ups_d0cf132fdfdc")
static char device_id[32] = {0};
static char mqtt_base_topic[64] = {0};
static uint8_t device_mac[6] = {0};

// Generate unique device ID from MAC address
static void generate_device_id(void)
{
    esp_efuse_mac_get_default(device_mac);

    snprintf(device_id, sizeof(device_id), "apc_ups_%02x%02x%02x%02x%02x%02x",
             device_mac[0], device_mac[1], device_mac[2],
             device_mac[3], device_mac[4], device_mac[5]);

    snprintf(mqtt_base_topic, sizeof(mqtt_base_topic), "homeassistant/sensor/%s", device_id);

    ESP_LOGI(TAG, "📱 Device ID: %s", device_id);
    ESP_LOGI(TAG, "📱 MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
             device_mac[0], device_mac[1], device_mac[2],
             device_mac[3], device_mac[4], device_mac[5]);
    ESP_LOGI(TAG, "📡 MQTT Base Topic: %s", mqtt_base_topic);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "✅ MQTT connected to broker");
        mqtt_connected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "⚠️ MQTT disconnected");
        mqtt_connected = false;
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "❌ MQTT error");
        break;
    default:
        break;
    }
}

esp_err_t mqtt_init(void)
{
    // Generate unique device ID from MAC address
    generate_device_id();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_BROKER_URL,
        .credentials.username = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(mqtt_client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started with username: %s", CONFIG_MQTT_USERNAME);
    return ESP_OK;
}

esp_err_t mqtt_publish_metric(const char *sensor_name, float value, const char *unit)
{
    if (!mqtt_connected || mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    char payload[64];

    snprintf(topic, sizeof(topic), "%s/%s/state", mqtt_base_topic, sensor_name);
    snprintf(payload, sizeof(payload), "%.2f", value);

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mqtt_publish_string(const char *sensor_name, const char *value)
{
    if (!mqtt_connected || mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s/state", mqtt_base_topic, sensor_name);

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, value, 0, 1, 0);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mqtt_publish_discovery(const char *sensor_name, const char *friendly_name, const char *unit, const char *device_class)
{
    if (!mqtt_connected || mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[256];
    char payload[1024];

    // Discovery topic: homeassistant/sensor/<device_id>/<sensor_name>/config
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", device_id, sensor_name);

    // Build discovery payload with unique device ID
    snprintf(payload, sizeof(payload),
        "{"
        "\"name\":\"%s\","
        "\"state_topic\":\"%s/%s/state\","
        "\"unique_id\":\"%s_%s\","
        "\"device\":{"
            "\"identifiers\":[\"%s\"],"
            "\"name\":\"APC UPS (%02X:%02X:%02X:%02X:%02X:%02X)\","
            "\"manufacturer\":\"APC\","
            "\"model\":\"Back-UPS XS 1000M\""
        "}",
        friendly_name, mqtt_base_topic, sensor_name, device_id, sensor_name,
        device_id,
        device_mac[0], device_mac[1], device_mac[2],
        device_mac[3], device_mac[4], device_mac[5]
    );

    if (unit != NULL && strlen(unit) > 0) {
        char unit_field[64];
        snprintf(unit_field, sizeof(unit_field), ",\"unit_of_measurement\":\"%s\"", unit);
        strcat(payload, unit_field);
    }

    if (device_class != NULL && strlen(device_class) > 0) {
        char class_field[64];
        snprintf(class_field, sizeof(class_field), ",\"device_class\":\"%s\"", device_class);
        strcat(payload, class_field);
    }

    strcat(payload, "}");

    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish discovery for %s", sensor_name);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Published discovery for %s", sensor_name);
    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    return mqtt_connected;
}
