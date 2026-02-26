#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "apc_hid_parser.h"
#include "usb_host_manager.h"

static const char *TAG = "main";

// Task to publish UPS metrics periodically
static void mqtt_publish_task(void *arg)
{
    ESP_LOGI(TAG, "📊 MQTT publish task started");

    // Wait for MQTT connection
    while (!mqtt_is_connected()) {
        ESP_LOGI(TAG, "Waiting for MQTT connection...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Publish Home Assistant discovery configs
    ESP_LOGI(TAG, "📡 Publishing MQTT discovery configs...");
    ESP_LOGI(TAG, "💡 Each UPS bridge has unique device ID based on MAC address");

    // Battery metrics
    mqtt_publish_discovery("battery_charge", "Battery Charge", "%", "battery");
    mqtt_publish_discovery("battery_voltage", "Battery Voltage", "V", "voltage");
    mqtt_publish_discovery("battery_voltage_nominal", "Battery Nominal Voltage", "V", "voltage");
    mqtt_publish_discovery("battery_runtime", "Battery Runtime", "s", "duration");
    mqtt_publish_discovery("battery_runtime_low", "Battery Low Runtime", "s", "duration");
    mqtt_publish_discovery("battery_charge_low", "Battery Low Charge", "%", "battery");
    mqtt_publish_discovery("battery_charge_warning", "Battery Warning Charge", "%", "battery");
    mqtt_publish_discovery("battery_type", "Battery Type", NULL, NULL);
    mqtt_publish_discovery("battery_mfr_date", "Battery Manufacture Date", NULL, NULL);

    // Input power metrics
    mqtt_publish_discovery("input_voltage", "Input Voltage", "V", "voltage");
    mqtt_publish_discovery("input_voltage_nominal", "Input Nominal Voltage", "V", "voltage");
    // NOTE: input_frequency not available - UPS reports 0 Hz (hardware limitation)
    // mqtt_publish_discovery("input_frequency", "Input Frequency", "Hz", "frequency");
    mqtt_publish_discovery("input_transfer_low", "Low Voltage Transfer", "V", "voltage");
    mqtt_publish_discovery("input_transfer_high", "High Voltage Transfer", "V", "voltage");
    mqtt_publish_discovery("input_sensitivity", "Input Sensitivity", NULL, NULL);
    mqtt_publish_discovery("input_transfer_reason", "Last Transfer Reason", NULL, NULL);

    // Output/Load metrics
    // NOTE: output_voltage not available - line-interactive UPS doesn't measure output (hardware limitation)
    // mqtt_publish_discovery("output_voltage", "Output Voltage", "V", "voltage");
    mqtt_publish_discovery("load_percent", "Load", "%", "power_factor");
    mqtt_publish_discovery("nominal_power", "Nominal Power", "W", "power");

    // UPS status and timers
    mqtt_publish_discovery("status", "UPS Status", NULL, NULL);
    mqtt_publish_discovery("beeper_status", "Beeper Status", NULL, NULL);
    // Note: delay_shutdown removed - not available in HID reports
    // mqtt_publish_discovery("delay_shutdown", "Shutdown Delay", "s", "duration");
    mqtt_publish_discovery("delay_reboot", "Reboot Delay", "s", "duration");
    mqtt_publish_discovery("reboot_timer", "Reboot Timer", "s", "duration");
    mqtt_publish_discovery("shutdown_timer", "Shutdown Timer", "s", "duration");
    mqtt_publish_discovery("self_test_result", "Self-Test Result", NULL, NULL);

    // Device information
    // Note: firmware_version not available - requires USB string descriptors
    // mqtt_publish_discovery("firmware_version", "Firmware Version", NULL, NULL);
    mqtt_publish_discovery("driver_name", "Driver Name", NULL, NULL);
    mqtt_publish_discovery("driver_version", "Driver Version", NULL, NULL);
    mqtt_publish_discovery("driver_state", "Driver State", NULL, NULL);
    mqtt_publish_discovery("power_failure", "Power Failure", NULL, NULL);

    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        if (mqtt_is_connected()) {
            const ups_metrics_t *metrics = apc_hid_get_metrics();
            
            if (metrics->valid) {
                ESP_LOGI(TAG, "═══════════════════════════════════════════");
                ESP_LOGI(TAG, "📤 PUBLISHING TO MQTT");
                ESP_LOGI(TAG, "   Broker: %s", CONFIG_MQTT_BROKER_URL);
                ESP_LOGI(TAG, "   Base Topic: homeassistant/sensor/apc_ups");
                ESP_LOGI(TAG, "");

                // Publish key metrics with detailed logging
                ESP_LOGI(TAG, "   📊 battery_charge → %.1f%%", metrics->battery_charge);
                mqtt_publish_metric("battery_charge", metrics->battery_charge, "%");

                ESP_LOGI(TAG, "   ⏱️  battery_runtime → %.0f seconds (%.1f min)",
                         metrics->battery_runtime, metrics->battery_runtime / 60.0f);
                mqtt_publish_metric("battery_runtime", metrics->battery_runtime, "s");

                ESP_LOGI(TAG, "   🔋 battery_voltage → %.1fV", metrics->battery_voltage);
                mqtt_publish_metric("battery_voltage", metrics->battery_voltage, "V");

                // Battery additional metrics
                if (metrics->battery_nominal_voltage > 0) {
                    mqtt_publish_metric("battery_voltage_nominal", metrics->battery_nominal_voltage, "V");
                }
                if (metrics->low_battery_runtime_threshold > 0) {
                    mqtt_publish_metric("battery_runtime_low", metrics->low_battery_runtime_threshold, "s");
                }
                if (metrics->low_battery_charge_threshold > 0) {
                    mqtt_publish_metric("battery_charge_low", metrics->low_battery_charge_threshold, "%");
                }
                if (metrics->battery_warning_threshold > 0) {
                    mqtt_publish_metric("battery_charge_warning", metrics->battery_warning_threshold, "%");
                }
                if (strlen(metrics->battery_type) > 0) {
                    mqtt_publish_string("battery_type", metrics->battery_type);
                }
                if (strlen(metrics->battery_mfr_date) > 0) {
                    mqtt_publish_string("battery_mfr_date", metrics->battery_mfr_date);
                }

                ESP_LOGI(TAG, "   ⚡ input_voltage → %.1fV", metrics->input_voltage);
                mqtt_publish_metric("input_voltage", metrics->input_voltage, "V");

                // Input power additional metrics
                if (metrics->input_voltage_nominal > 0) {
                    mqtt_publish_metric("input_voltage_nominal", metrics->input_voltage_nominal, "V");
                }
                // input_frequency removed - hardware doesn't support
                // if (metrics->input_frequency > 0) {
                //     ESP_LOGI(TAG, "   〰️ input_frequency → %.1fHz", metrics->input_frequency);
                //     mqtt_publish_metric("input_frequency", metrics->input_frequency, "Hz");
                // }
                if (metrics->low_voltage_transfer > 0) {
                    mqtt_publish_metric("input_transfer_low", metrics->low_voltage_transfer, "V");
                }
                if (metrics->high_voltage_transfer > 0) {
                    mqtt_publish_metric("input_transfer_high", metrics->high_voltage_transfer, "V");
                }
                if (strlen(metrics->input_sensitivity) > 0) {
                    mqtt_publish_string("input_sensitivity", metrics->input_sensitivity);
                }
                if (strlen(metrics->last_transfer_reason) > 0) {
                    mqtt_publish_string("input_transfer_reason", metrics->last_transfer_reason);
                }

                ESP_LOGI(TAG, "   📈 load_percent → %.1f%%", metrics->load_percent);
                mqtt_publish_metric("load_percent", metrics->load_percent, "%");

                // Output/Load additional metrics
                // output_voltage removed - hardware doesn't support
                // if (metrics->output_voltage > 0) {
                //     ESP_LOGI(TAG, "   ⚡ output_voltage → %.1fV", metrics->output_voltage);
                //     mqtt_publish_metric("output_voltage", metrics->output_voltage, "V");
                // }
                if (metrics->nominal_power > 0) {
                    ESP_LOGI(TAG, "   ⚡ nominal_power → %.0fW", metrics->nominal_power);
                    mqtt_publish_metric("nominal_power", metrics->nominal_power, "W");
                }

                ESP_LOGI(TAG, "   🚦 status → %s", metrics->status_string);
                mqtt_publish_string("status", metrics->status_string);

                // UPS configuration and timers
                if (strlen(metrics->beeper_status) > 0) {
                    mqtt_publish_string("beeper_status", metrics->beeper_status);
                }
                // Note: Report 0x11 is battery_charge_low, not shutdown_delay
                // Shutdown delay configuration not available in HID reports
                // (NUT gets it from different source or doesn't expose it)

                // Publish delay_before_reboot (Report 0x13) - configuration value
                if (metrics->delay_before_reboot > 0) {
                    mqtt_publish_metric("delay_reboot", metrics->delay_before_reboot, "s");
                }

                // Active timers (Report 0x17 = reboot, Report 0x15 = shutdown)
                // These can be negative (-1 = not active)
                mqtt_publish_metric("reboot_timer", metrics->reboot_timer, "s");
                mqtt_publish_metric("shutdown_timer", metrics->shutdown_timer, "s");

                // Self-test result
                if (strlen(metrics->self_test_result) > 0) {
                    mqtt_publish_string("self_test_result", metrics->self_test_result);
                }

                // Device information
                // firmware_version removed - not available in HID reports
                if (strlen(metrics->driver_name) > 0) {
                    mqtt_publish_string("driver_name", metrics->driver_name);
                }
                if (strlen(metrics->driver_version) > 0) {
                    mqtt_publish_string("driver_version", metrics->driver_version);
                }
                if (strlen(metrics->driver_state) > 0) {
                    mqtt_publish_string("driver_state", metrics->driver_state);
                }
                if (strlen(metrics->power_failure_status) > 0) {
                    mqtt_publish_string("power_failure", metrics->power_failure_status);
                }

                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "✅ MQTT PUBLISH COMPLETE");
                ESP_LOGI(TAG, "🔋 Summary: %s | Battery: %.0f%% | Load: %.0f%%",
                         metrics->status_string,
                         metrics->battery_charge,
                         metrics->load_percent);
                ESP_LOGI(TAG, "═══════════════════════════════════════════");
            } else {
                ESP_LOGW(TAG, "⚠️ No valid UPS metrics available");
            }
        } else {
            ESP_LOGW(TAG, "⚠️ MQTT not connected, skipping publish");
        }
        
        vTaskDelay(pdMS_TO_TICKS(CONFIG_MQTT_PUBLISH_INTERVAL_MS));
    }
}

// Simulated UPS data task (for testing without USB device)
static void simulate_ups_data_task(void *arg)
{
    ESP_LOGI(TAG, "🧪 Simulated UPS data task started (for testing)");
    
    ups_metrics_t test_metrics = {
        .battery_charge = 100.0f,
        .battery_voltage = 13.7f,
        .battery_runtime = 2420.0f,
        .input_voltage = 121.0f,
        .load_percent = 14.0f,
        .status = {
            .online = true,
            .discharging = false,
            .charging = false,
            .low_battery = false,
        },
        .valid = true,
    };
    
    apc_hid_format_status(&test_metrics.status, test_metrics.status_string, 
                         sizeof(test_metrics.status_string));
    
    while (1) {
        // Simulate slight variations
        test_metrics.battery_charge = 95.0f + (esp_random() % 6);
        test_metrics.load_percent = 10.0f + (esp_random() % 10);
        test_metrics.input_voltage = 118.0f + ((esp_random() % 5) * 0.5f);
        
        // Update metrics
        apc_hid_parse_report(0x0C, (uint8_t*)&test_metrics, sizeof(test_metrics), &test_metrics);
        
        vTaskDelay(pdMS_TO_TICKS(CONFIG_UPS_POLL_INTERVAL_MS));
    }
}

// Build timestamp - updated on every compile
#define BUILD_TIMESTAMP __DATE__ " " __TIME__
#define FIRMWARE_VERSION "1.10.0"

void app_main(void)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "🚀 APC USB-MQTT Bridge Starting");
    ESP_LOGI(TAG, "   Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "   Build: %s", BUILD_TIMESTAMP);
    ESP_LOGI(TAG, "═══════════════════════════════════════════");

    // BOOT DELAY: Give 10 seconds to flash new firmware before USB host takes over
    ESP_LOGW(TAG, "⏳ Boot delay: 10 seconds for firmware update window...");
    for (int i = 10; i > 0; i--) {
        ESP_LOGI(TAG, "⏱️  %d seconds remaining (press RESET to abort and stay in programming mode)", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "✅ Boot delay complete, continuing...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize HID parser
    apc_hid_parser_init();

    // Initialize WiFi
    ESP_LOGI(TAG, "📶 Initializing WiFi...");
    ESP_ERROR_CHECK(wifi_init_sta());

    // Wait for WiFi connection
    if (wifi_wait_connected(30000) != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to connect to WiFi, restarting...");
        esp_restart();
    }

    // Initialize MQTT
    ESP_LOGI(TAG, "📡 Initializing MQTT...");
    ESP_ERROR_CHECK(mqtt_init());
    ESP_LOGI(TAG, "DEBUG: MQTT init complete");

    // Initialize USB Host
    ESP_LOGI(TAG, "DEBUG: About to init USB Host");
    ESP_LOGI(TAG, "🔌 Initializing USB Host on GPIO19/20...");
    esp_err_t usb_err = usb_host_init();
    ESP_LOGI(TAG, "DEBUG: usb_host_init returned: 0x%x (%s)", usb_err, esp_err_to_name(usb_err));

    if (usb_err == ESP_OK) {
        ESP_LOGI(TAG, "✅ USB Host initialized, creating USB task");
        xTaskCreate(usb_host_task, "usb_host", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGW(TAG, "⚠️ USB Host init failed: %s, falling back to simulated data", esp_err_to_name(usb_err));
        xTaskCreate(simulate_ups_data_task, "simulate_ups", 2048, NULL, 3, NULL);
    }

    ESP_LOGI(TAG, "DEBUG: USB setup complete, creating MQTT publish task");
    // Create MQTT publish task
    xTaskCreate(mqtt_publish_task, "mqtt_publish", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "=== ✅ APC USB-MQTT Bridge Running ===");
    ESP_LOGI(TAG, "WiFi: Connected to %s", CONFIG_WIFI_SSID);
    ESP_LOGI(TAG, "MQTT Broker: %s", CONFIG_MQTT_BROKER_URL);
#ifdef DISABLE_USB_HOST
    ESP_LOGW(TAG, "🐛 DEBUG MODE: USB Host disabled, using simulated data only");
#endif
}
