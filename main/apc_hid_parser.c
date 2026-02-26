#include "apc_hid_parser.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "apc_hid_parser";
static ups_metrics_t current_metrics = {0};

void apc_hid_parser_init(void)
{
    memset(&current_metrics, 0, sizeof(ups_metrics_t));
    current_metrics.valid = false;

    // Set default values
    strcpy(current_metrics.driver_name, "esp32-usb-hid");
    strcpy(current_metrics.driver_version, "1.0.0");
    strcpy(current_metrics.driver_state, "running");
    strcpy(current_metrics.battery_type, "PbAc");
    strcpy(current_metrics.power_failure_status, "OK");

    ESP_LOGI(TAG, "🔋 APC HID parser initialized");
}

// Helper function to print hex dump
static void log_hex_dump(const char *prefix, const uint8_t *data, size_t length)
{
    char hex_str[256];
    char ascii_str[64];
    int hex_pos = 0;
    int ascii_pos = 0;

    for (size_t i = 0; i < length && hex_pos < sizeof(hex_str) - 4; i++) {
        hex_pos += snprintf(hex_str + hex_pos, sizeof(hex_str) - hex_pos, "%02X ", data[i]);
        ascii_str[ascii_pos++] = (data[i] >= 32 && data[i] <= 126) ? data[i] : '.';
    }
    ascii_str[ascii_pos] = '\0';

    ESP_LOGI(TAG, "%s [%d bytes]: %s | %s", prefix, length, hex_str, ascii_str);
}

bool apc_hid_parse_report(uint8_t report_id, const uint8_t *data, size_t length, ups_metrics_t *metrics)
{
    if (data == NULL || length == 0) {
        return false;
    }

    // Log raw HID report
    ESP_LOGI(TAG, "═══════════════════════════════════════════");
    ESP_LOGI(TAG, "📦 RAW HID REPORT");
    ESP_LOGI(TAG, "   Report ID: 0x%02X (%d)", report_id, report_id);
    log_hex_dump("   Data", data, length);

    // Use current_metrics if metrics pointer is NULL
    ups_metrics_t *target = (metrics != NULL) ? metrics : &current_metrics;
    bool updated = false;

    ESP_LOGI(TAG, "🔍 PARSING LOGIC:");
    switch (report_id) {
        case 0x0C:  // Battery charge and runtime (UPS.PowerSummary)
            ESP_LOGI(TAG, "   Type: Battery Charge & Runtime (UPS.PowerSummary)");
            if (length >= 4) {
                target->battery_charge = (float)data[1];
                uint16_t runtime_seconds = data[2] | (data[3] << 8);
                target->battery_runtime = (float)runtime_seconds;

                ESP_LOGI(TAG, "   ├─ Byte[1]: Battery charge = %d%%", data[1]);
                ESP_LOGI(TAG, "   ├─ Byte[2-3]: Runtime = %d seconds (%.1f min)", runtime_seconds, runtime_seconds / 60.0f);
                ESP_LOGI(TAG, "   └─ Result: Battery %.0f%%, Runtime %.0fs",
                         target->battery_charge, target->battery_runtime);
                updated = true;
            }
            break;

        case 0x06:  // Status flags
            ESP_LOGI(TAG, "   Type: Status Flags");
            if (length >= 4) {
                uint8_t status_byte = data[3];
                target->status.online = (status_byte & 0x08) != 0;
                target->status.discharging = (status_byte & 0x01) != 0;
                target->status.charging = (status_byte & 0x02) != 0;
                target->status.low_battery = (status_byte & 0x04) != 0;

                ESP_LOGI(TAG, "   └─ Status byte 0x%02X: %s", status_byte,
                         target->status.online ? "ONLINE" : "ON_BATTERY");
                updated = true;
            }
            break;

        case 0x08:  // Battery nominal voltage (UPS.PowerSummary.ConfigVoltage)
            ESP_LOGI(TAG, "   Type: Battery Nominal Voltage");
            if (length >= 3) {
                // 16-bit value with Exponent = -2, so divide by 100
                // Raw data example: 08 B0 04 = 0x04B0 = 1200 / 100 = 12V
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->battery_nominal_voltage = (float)voltage_raw / 100.0f;
                ESP_LOGI(TAG, "   └─ Raw: 0x%04X → %.1fV", voltage_raw, target->battery_nominal_voltage);
                updated = true;
            }
            break;

        case 0x09:  // Battery voltage (UPS.PowerSummary.Voltage) - Feature Report
            ESP_LOGI(TAG, "   Type: Battery Voltage");
            if (length >= 3) {
                // From NUT: 16-bit value, Exponent = -2, so divide by 100
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->battery_voltage = (float)voltage_raw / 100.0f;
                ESP_LOGI(TAG, "   └─ Raw: 0x%04X → %.2fV", voltage_raw, target->battery_voltage);
                updated = true;
            }
            break;

        case 0x0B:  // Battery nominal voltage
            ESP_LOGI(TAG, "   Type: Battery Nominal Voltage");
            if (length >= 2) {
                target->battery_nominal_voltage = (float)data[1];
                ESP_LOGI(TAG, "   └─ Nominal: %.0fV", target->battery_nominal_voltage);
                updated = true;
            }
            break;

        case 0x0D:  // Battery voltage
            ESP_LOGI(TAG, "   Type: Battery Voltage");
            if (length >= 2) {
                target->battery_voltage = (float)data[1] / 10.0f;
                ESP_LOGI(TAG, "   └─ Battery: %.1fV", target->battery_voltage);
                updated = true;
            }
            break;

        case 0x0E:  // Full charge capacity (NOT low battery threshold!)
            ESP_LOGI(TAG, "   Type: Full Charge Capacity");
            if (length >= 2) {
                // This is FullChargeCapacity = 100%, not the low battery threshold
                // Don't store this as low_battery_charge_threshold
                ESP_LOGI(TAG, "   └─ Full Capacity: %.0f%% (not low threshold)", (float)data[1]);
                // Do NOT set updated = true, we don't want to store this
            }
            break;

        case 0x0F:  // Battery warning threshold
            ESP_LOGI(TAG, "   Type: Battery Warning Threshold");
            if (length >= 2) {
                target->battery_warning_threshold = (float)data[1];
                ESP_LOGI(TAG, "   └─ Threshold: %.0f%%", target->battery_warning_threshold);
                updated = true;
            }
            break;

        case 0x10:  // Beeper status
            ESP_LOGI(TAG, "   Type: Beeper Status");
            if (length >= 2) {
                const char *beeper_state[] = {"disabled", "enabled", "muted"};
                uint8_t beeper_val = data[1];
                if (beeper_val < 3) {
                    strncpy(target->beeper_status, beeper_state[beeper_val], sizeof(target->beeper_status) - 1);
                    ESP_LOGI(TAG, "   └─ Beeper: %s", target->beeper_status);
                    updated = true;
                }
            }
            break;

        case 0x11:  // Battery low charge threshold (UPS.PowerSummary.RemainingCapacityLimit)
            ESP_LOGI(TAG, "   Type: Battery Low Charge Threshold");
            if (length >= 2) {
                target->low_battery_charge_threshold = (float)data[1];
                ESP_LOGI(TAG, "   └─ Threshold: %.0f%%", target->low_battery_charge_threshold);
                updated = true;
            }
            break;

        case 0x12:  // Low battery runtime threshold
            ESP_LOGI(TAG, "   Type: Low Battery Runtime Threshold");
            if (length >= 3) {
                uint16_t runtime = data[1] | (data[2] << 8);
                target->low_battery_runtime_threshold = (float)runtime;
                ESP_LOGI(TAG, "   └─ Threshold: %.0fs", target->low_battery_runtime_threshold);
                updated = true;
            }
            break;

        case 0x15:  // Shutdown timer
            ESP_LOGI(TAG, "   Type: Shutdown Timer");
            if (length >= 3) {
                int16_t timer = (int16_t)(data[1] | (data[2] << 8));
                target->shutdown_timer = (float)timer;
                ESP_LOGI(TAG, "   └─ Timer: %.0fs", target->shutdown_timer);
                updated = true;
            }
            break;

        case 0x16:  // Status bits (PresentStatus)
            ESP_LOGI(TAG, "   Type: Present Status Bits");
            if (length >= 2) {
                uint8_t present_status = data[1];
                target->status.online = (present_status & 0x01) != 0;
                target->status.discharging = (present_status & 0x02) != 0;
                target->status.charging = (present_status & 0x04) != 0;
                target->status.low_battery = (present_status & 0x08) != 0;
                target->status.overload = (present_status & 0x10) != 0;
                target->status.replace_battery = (present_status & 0x20) != 0;
                target->status.boost = (present_status & 0x40) != 0;
                target->status.trim = (present_status & 0x80) != 0;

                ESP_LOGI(TAG, "   └─ Status: 0x%02X [%s%s%s%s%s%s%s%s]", present_status,
                         target->status.online ? "OL " : "",
                         target->status.discharging ? "DISCHRG " : "",
                         target->status.charging ? "CHRG " : "",
                         target->status.low_battery ? "LB " : "",
                         target->status.overload ? "OVER " : "",
                         target->status.replace_battery ? "RB " : "",
                         target->status.boost ? "BOOST " : "",
                         target->status.trim ? "TRIM" : "");
                updated = true;
            }
            break;

        case 0x17:  // Reboot timer
            ESP_LOGI(TAG, "   Type: Reboot Timer");
            if (length >= 3) {
                uint16_t timer = data[1] | (data[2] << 8);
                target->reboot_timer = (float)timer;
                ESP_LOGI(TAG, "   └─ Timer: %.0fs", target->reboot_timer);
                updated = true;
            }
            break;

        case 0x18:  // Self-test result
            ESP_LOGI(TAG, "   Type: Self-Test Result");
            if (length >= 2) {
                const char *test_results[] = {
                    "No test initiated",
                    "Test passed",
                    "Test in progress",
                    "General test failed",
                    "Battery failed",
                    "Deep battery test failed",
                    "Test aborted"
                };
                uint8_t test_val = data[1];
                if (test_val < 7) {
                    strncpy(target->self_test_result, test_results[test_val], sizeof(target->self_test_result) - 1);
                    ESP_LOGI(TAG, "   └─ Result: %s", target->self_test_result);
                    updated = true;
                }
            }
            break;

        case 0x1C:  // Battery manufacture date
            ESP_LOGI(TAG, "   Type: Battery Manufacture Date");
            if (length >= 4) {
                // Date stored as 3 bytes: Year (2 bytes), Month, Day
                uint16_t year = data[1] | (data[2] << 8);
                uint8_t month = (length > 3) ? data[3] : 1;
                uint8_t day = (length > 4) ? data[4] : 1;
                snprintf(target->battery_mfr_date, sizeof(target->battery_mfr_date),
                         "%04d/%02d/%02d", year, month, day);
                ESP_LOGI(TAG, "   └─ Date: %s", target->battery_mfr_date);
                updated = true;
            }
            break;

        case 0x20:  // Battery manufacture date (UPS.Battery.ManufacturerDate)
            ESP_LOGI(TAG, "   Type: Battery Manufacture Date");
            if (length >= 3) {
                // 16-bit value = days since reference date (likely 1970-01-01)
                // Example: 21690 days ≈ 59 years from 1970 = year 2029
                uint16_t days = data[1] | (data[2] << 8);
                snprintf(target->battery_mfr_date, sizeof(target->battery_mfr_date),
                         "%d days", days);
                ESP_LOGI(TAG, "   └─ Date: %d days since reference (raw data)", days);
                updated = true;
            }
            break;

        case 0x13:  // Delay before reboot (APCDelayBeforeReboot)
            ESP_LOGI(TAG, "   Type: Delay Before Reboot");
            if (length >= 2) {
                target->delay_before_reboot = (float)data[1];
                ESP_LOGI(TAG, "   └─ Delay: %.0f seconds", target->delay_before_reboot);
                updated = true;
            }
            break;

        case 0x14:  // Delay before shutdown (APCDelayBeforeShutdown)
            ESP_LOGI(TAG, "   Type: Delay Before Shutdown");
            if (length >= 2) {
                target->delay_before_shutdown = (float)data[1];
                ESP_LOGI(TAG, "   └─ Delay: %.0f seconds", target->delay_before_shutdown);
                updated = true;
            }
            break;

        case 0x24:  // Battery runtime low threshold (UPS.Battery.RemainingTimeLimit)
            ESP_LOGI(TAG, "   Type: Battery Runtime Low Threshold");
            if (length >= 3) {
                uint16_t runtime = data[1] | (data[2] << 8);
                target->low_battery_runtime_threshold = (float)runtime;
                ESP_LOGI(TAG, "   └─ Threshold: %d seconds (%.1f min)", runtime, (float)runtime / 60.0f);
                updated = true;
            }
            break;

        case 0x21:  // Last transfer reason
            ESP_LOGI(TAG, "   Type: Last Transfer Reason");
            if (length >= 2) {
                const char *reasons[] = {
                    "No transfer",
                    "High line voltage",
                    "Brownout",
                    "Blackout",
                    "Small momentary sag",
                    "Deep momentary sag",
                    "Small momentary spike",
                    "Large momentary spike",
                    "Self test",
                    "Input frequency out of range",
                    "Input voltage out of range"
                };
                uint8_t reason = data[1];
                if (reason < 11) {
                    strncpy(target->last_transfer_reason, reasons[reason], sizeof(target->last_transfer_reason) - 1);
                    ESP_LOGI(TAG, "   └─ Reason code %d: %s", reason, target->last_transfer_reason);
                    updated = true;
                } else {
                    ESP_LOGI(TAG, "   └─ Unknown reason code: %d", reason);
                }
            }
            break;

        case 0x25:  // Nominal power
            ESP_LOGI(TAG, "   Type: Nominal Power");
            if (length >= 3) {
                uint16_t power = data[1] | (data[2] << 8);
                target->nominal_power = (float)power;
                ESP_LOGI(TAG, "   └─ Power: %.0fW", target->nominal_power);
                updated = true;
            }
            break;

        case 0x30:  // Input nominal voltage (UPS.Input.ConfigVoltage) - Feature Report
            ESP_LOGI(TAG, "   Type: Input Nominal Voltage");
            if (length >= 2) {
                target->input_voltage_nominal = (float)data[1];
                ESP_LOGI(TAG, "   └─ Nominal: %.0fV", target->input_voltage_nominal);
                updated = true;
            }
            break;

        case 0x31:  // Input voltage (UPS.Input.Voltage) - Feature Report
            ESP_LOGI(TAG, "   Type: Input Voltage");
            if (length >= 3) {
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->input_voltage = (float)voltage_raw;
                ESP_LOGI(TAG, "   └─ Raw: 0x%04X → %.0fV", voltage_raw, target->input_voltage);
                updated = true;
            }
            break;

        case 0x32:  // Low voltage transfer (UPS.Input.LowVoltageTransfer) - Feature Report
            ESP_LOGI(TAG, "   Type: Low Voltage Transfer");
            if (length >= 3) {
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->low_voltage_transfer = (float)voltage_raw;
                ESP_LOGI(TAG, "   └─ Transfer point: %.0fV", target->low_voltage_transfer);
                updated = true;
            }
            break;

        case 0x33:  // High voltage transfer (UPS.Input.HighVoltageTransfer) - Feature Report
            ESP_LOGI(TAG, "   Type: High Voltage Transfer");
            if (length >= 3) {
                uint16_t voltage_raw = data[1] | (data[2] << 8);
                target->high_voltage_transfer = (float)voltage_raw;
                ESP_LOGI(TAG, "   └─ Transfer point: %.0fV", target->high_voltage_transfer);
                updated = true;
            }
            break;

        case 0x50:  // Load percentage (UPS.PowerConverter.PercentLoad) - Feature Report
            ESP_LOGI(TAG, "   Type: Load Percentage");
            if (length >= 2) {
                target->load_percent = (float)data[1];
                ESP_LOGI(TAG, "   └─ Load: %.0f%%", target->load_percent);
                updated = true;
            }
            break;

        case 0x35:  // Input sensitivity
            ESP_LOGI(TAG, "   Type: Input Sensitivity");
            if (length >= 2) {
                const char *sensitivity[] = {"low", "medium", "high"};
                uint8_t sens_val = data[1];
                if (sens_val < 3) {
                    strncpy(target->input_sensitivity, sensitivity[sens_val], sizeof(target->input_sensitivity) - 1);
                    ESP_LOGI(TAG, "   └─ Sensitivity: %s", target->input_sensitivity);
                    updated = true;
                }
            }
            break;

        case 0x36:  // Input frequency (UPS.Input.Frequency) - Feature Report
            ESP_LOGI(TAG, "   Type: Input Frequency");
            if (length >= 2) {
                // Frequency is typically 50 or 60 Hz
                // NOTE: This UPS reports 0 Hz - frequency might not be available
                // or encoded differently. Only update if non-zero.
                if (data[1] > 0) {
                    target->input_frequency = (float)data[1];
                    ESP_LOGI(TAG, "   └─ Frequency: %.0fHz", target->input_frequency);
                    updated = true;
                } else {
                    ESP_LOGI(TAG, "   └─ Frequency: Not available (0 Hz reported)");
                }
            }
            break;

        case 0x03:  // Battery chemistry/type (UPS.PowerSummary.iDeviceChemistry)
            ESP_LOGI(TAG, "   Type: Battery Chemistry");
            if (length >= 2) {
                // Common values: 1=PbAc (Lead Acid), 2=Li-ion, 3=NiCd, 4=NiMH
                // NOTE: APC typically uses PbAc but this UPS reports code 4 (NiMH)
                // Mapping might be vendor-specific. Reporting as-is.
                const char *chemistry[] = {"Unknown", "PbAc", "Li-ion", "NiCd", "NiMH"};
                uint8_t chem_val = data[1];
                if (chem_val < 5) {
                    strncpy(target->battery_type, chemistry[chem_val], sizeof(target->battery_type) - 1);
                    ESP_LOGI(TAG, "   └─ Chemistry code: %d → %s", chem_val, target->battery_type);
                    updated = true;
                } else {
                    ESP_LOGW(TAG, "   └─ Unknown chemistry code: %d", chem_val);
                }
            }
            break;

        case 0x07:  // UPS manufacture date (or unknown field)
            ESP_LOGI(TAG, "   Type: UPS Manufacture Date");
            // NOTE: This report returns only 3 bytes: 07 D6 54
            // Interpreting as date gives nonsensical year 21718
            // Likely this is NOT a date field or uses different encoding
            // Same data as Report 0x20 - both appear to be unidentified fields
            if (length >= 3) {
                ESP_LOGI(TAG, "   └─ Raw data: 0x%02X%02X%02X (unimplemented)",
                         data[1], data[2], (length > 2) ? data[3] : 0);
                ESP_LOGW(TAG, "   └─ Date parsing not implemented (insufficient data or wrong field)");
            }
            break;

        case 0x34:  // Input sensitivity adjustment (writable setting)
            ESP_LOGI(TAG, "   Type: Input Sensitivity Adjustment");
            if (length >= 2) {
                ESP_LOGI(TAG, "   └─ Adjustment value: %d", data[1]);
                updated = true;
            }
            break;

        case 0x52:  // Nominal real power (UPS.PowerSummary.ConfigActivePower)
            ESP_LOGI(TAG, "   Type: Nominal Real Power");
            if (length >= 3) {
                uint16_t power = data[1] | (data[2] << 8);
                target->nominal_power = (float)power;
                ESP_LOGI(TAG, "   └─ Real Power: %.0fW", target->nominal_power);
                updated = true;
            }
            break;

        case 0x60:  // Firmware version (part of string)
            ESP_LOGI(TAG, "   Type: Firmware Version");
            if (length >= 2) {
                // Firmware version is often split across multiple reports or encoded
                snprintf(target->firmware_version, sizeof(target->firmware_version), "%d.%d", data[1], data[2]);
                ESP_LOGI(TAG, "   └─ Version: %s", target->firmware_version);
                updated = true;
            }
            break;

        default:
            ESP_LOGI(TAG, "   Type: ❓ UNKNOWN Report ID (0x%02X)", report_id);
            ESP_LOGI(TAG, "   └─ This report ID is not yet handled");
            break;
    }

    if (updated) {
        target->last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        target->valid = true;

        // Update status string
        apc_hid_format_status(&target->status, target->status_string,
                             sizeof(target->status_string));

        ESP_LOGI(TAG, "✅ METRICS UPDATED");
        ESP_LOGI(TAG, "   Status: %s", target->status_string);
        ESP_LOGI(TAG, "═══════════════════════════════════════════");

        // If we updated current_metrics, copy to output if provided
        if (target == &current_metrics && metrics != NULL) {
            memcpy(metrics, &current_metrics, sizeof(ups_metrics_t));
        }
    } else {
        ESP_LOGI(TAG, "⚠️  NO UPDATE (insufficient data or parsing issue)");
        ESP_LOGI(TAG, "═══════════════════════════════════════════");
    }

    return updated;
}

const ups_metrics_t* apc_hid_get_metrics(void)
{
    return &current_metrics;
}

void apc_hid_format_status(const ups_status_t *status, char *buffer, size_t buffer_size)
{
    buffer[0] = '\0';

    if (status->online) {
        strncat(buffer, "OL", buffer_size - strlen(buffer) - 1);  // Online
    } else if (status->discharging) {
        strncat(buffer, "OB", buffer_size - strlen(buffer) - 1);  // On Battery
    }

    if (status->charging) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "CHRG", buffer_size - strlen(buffer) - 1);
    }

    if (status->low_battery) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "LB", buffer_size - strlen(buffer) - 1);
    }

    if (status->overload) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "OVER", buffer_size - strlen(buffer) - 1);
    }

    if (status->replace_battery) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "RB", buffer_size - strlen(buffer) - 1);
    }

    if (status->boost) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "BOOST", buffer_size - strlen(buffer) - 1);
    }

    if (status->trim) {
        if (strlen(buffer) > 0) strncat(buffer, " ", buffer_size - strlen(buffer) - 1);
        strncat(buffer, "TRIM", buffer_size - strlen(buffer) - 1);
    }

    if (strlen(buffer) == 0) {
        strncat(buffer, "UNKNOWN", buffer_size - 1);
    }
}
