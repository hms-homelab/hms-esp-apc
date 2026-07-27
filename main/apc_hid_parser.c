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

/* APC pads its string descriptors with spaces on both ends. */
static void trim_copy(char *dst, size_t dst_size, const char *src)
{
    while (*src == ' ') src++;
    size_t n = strlen(src);
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t')) n--;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void apc_hid_set_string(apc_string_id_t which, const char *value)
{
    if (value == NULL || value[0] == '\0') return;

    char clean[64];
    trim_copy(clean, sizeof(clean), value);
    if (clean[0] == '\0') return;
    value = clean;

    switch (which) {
        case APC_STR_MANUFACTURER:
            strlcpy(current_metrics.ups_manufacturer, value,
                    sizeof(current_metrics.ups_manufacturer));
            break;
        case APC_STR_MODEL: {
            /* iProduct on APC reads e.g.
             *   "Back-UPS XS 1000M FW:945.d11 .D USB FW:"
             * i.e. model + firmware crammed into one string. NUT splits this the
             * same way; keep only the model, and take the firmware from the
             * "FW:" segment if string 7 did not supply one. */
            char model[sizeof(current_metrics.ups_model)];
            const char *fw = strstr(value, " FW:");
            if (fw != NULL) {
                size_t n = (size_t)(fw - value);
                if (n >= sizeof(model)) n = sizeof(model) - 1;
                memcpy(model, value, n);
                model[n] = '\0';

                if (current_metrics.firmware_version[0] == '\0') {
                    char fwbuf[sizeof(current_metrics.firmware_version)];
                    strlcpy(fwbuf, fw + 4, sizeof(fwbuf));
                    char *tail = strstr(fwbuf, " USB FW:");
                    if (tail) *tail = '\0';
                    trim_copy(current_metrics.firmware_version,
                              sizeof(current_metrics.firmware_version), fwbuf);
                }
            } else {
                strlcpy(model, value, sizeof(model));
            }
            trim_copy(current_metrics.ups_model, sizeof(current_metrics.ups_model), model);
            break;
        }
        case APC_STR_SERIAL:
            strlcpy(current_metrics.ups_serial, value, sizeof(current_metrics.ups_serial));
            break;
        case APC_STR_CHEMISTRY:
            /* The real chemistry, e.g. "PbAc". Report 0x03 only carries the
             * string INDEX; decoding that index as a chemistry enum is what
             * produced the bogus "NiMH" on a lead-acid UPS. */
            strlcpy(current_metrics.battery_type, value,
                    sizeof(current_metrics.battery_type));
            break;
        case APC_STR_FIRMWARE:
            strlcpy(current_metrics.firmware_version, value,
                    sizeof(current_metrics.firmware_version));
            break;
    }
    ESP_LOGI(TAG, "🏷️  String[%d] = \"%s\"", (int)which, value);
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

/* HID Power Device Class packed date: bits 15..9 = year-1980, 8..5 = month,
 * 4..0 = day. The old code printed the raw word as "N days since reference",
 * which is why battery_mfr_date read "21690 days" instead of a date. */
static void format_hid_date(char *dst, size_t dst_size, uint16_t packed)
{
    unsigned year  = 1980u + (packed >> 9);
    unsigned month = (packed >> 5) & 0x0F;
    unsigned day   = packed & 0x1F;

    if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
        snprintf(dst, dst_size, "%04u/%02u/%02u", year, month, day);
    } else {
        snprintf(dst, dst_size, "raw 0x%04X", packed);
    }
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

        case 0x06:  // Charging / Discharging as byte-wide booleans
            // Per the report descriptor this is NOT a bitfield. Report Size is
            // still 8 here, so it is three 8-bit fields:
            //   data[1] = Battery.Charging       (0/1)
            //   data[2] = Battery.Discharging    (0/1)
            //   data[3] = vendor page 0xFF86 usage 0x60 — opaque, NOT status bits
            // The old code read data[3] as a bitfield; the vendor byte reads 0x08
            // which collided with its assumed "online" bit, so this report used to
            // assert OL and clear CHRG/LB, fighting report 0x16 for the same flags.
            ESP_LOGI(TAG, "   Type: Charging/Discharging Flags");
            if (length >= 3) {
                target->status.charging = (data[1] != 0);
                target->status.discharging = (data[2] != 0);

                ESP_LOGI(TAG, "   ├─ Byte[1]: Charging = %d", target->status.charging);
                ESP_LOGI(TAG, "   ├─ Byte[2]: Discharging = %d", target->status.discharging);
                if (length >= 4) {
                    // Vendor 0xFF86:0x60 = APCStatusFlag (NUT: 8/16 => on, 0 => off)
                    ESP_LOGI(TAG, "   └─ Byte[3]: APCStatusFlag = 0x%02X (%s)",
                             data[3], (data[3] == 0) ? "UPS OFF" : "UPS ON");
                }
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

        case 0x10:  // Battery.CapacityGranularity2 (NOT the beeper)
            // The descriptor declares this as Battery page usage 0x8E. It was
            // being published as beeper_status; the real beeper is report 0x18.
            ESP_LOGI(TAG, "   Type: Capacity Granularity 2");
            if (length >= 2) {
                ESP_LOGI(TAG, "   └─ Granularity: %d (not published)", data[1]);
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

        case 0x12:  // Battery.CapacityGranularity1 (NOT a runtime threshold)
            // Descriptor says Battery page usage 0x8D, a single byte. The real
            // low-runtime threshold is report 0x24 (RemainingTimeLimit).
            ESP_LOGI(TAG, "   Type: Capacity Granularity 1");
            if (length >= 2) {
                ESP_LOGI(TAG, "   └─ Granularity: %d (not published)", data[1]);
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

        case 0x16:  // PresentStatus bitfield (UPS.PowerSummary.PresentStatus)
            // Bit assignments taken from the device's own report descriptor, in
            // declaration order (Report Size 1, LSB first), NOT guessed:
            //   bit0  Battery.Charging                     (0x85:0x44)
            //   bit1  Battery.Discharging                  (0x85:0x45)
            //   bit2  Battery.ACPresent                    (0x85:0xD0)
            //   bit3  Battery.BatteryPresent               (0x85:0xD1)
            //   bit4  Battery.BelowRemainingCapacityLimit  (0x85:0x42)
            //   bit5  Power.ShutdownImminent               (0x84:0x69)
            //   bit6  Battery.RemainingTimeLimitExpired    (0x85:0x43)
            //   bit7  Power 0x84:0x73                      (unverified)
            //   bit8  Battery.NeedReplacement              (0x85:0x4B)
            //   bit9  Power.Overload                       (0x84:0x65)
            //   bit10 Battery 0x85:0xDB                    (unverified)
            //   bits 11-31 constant padding
            //
            // The old map was shifted one position down, so BatteryPresent —
            // which is set on every healthy UPS that has a battery in it — was
            // read as low_battery. That is where the permanent bogus "LB" came
            // from. Overload and NeedReplacement live above bit 7 and were never
            // reachable from a single byte at all.
            ESP_LOGI(TAG, "   Type: Present Status Bits");
            if (length >= 3) {
                uint16_t present_status = data[1] | (data[2] << 8);

                target->status.charging        = (present_status & 0x0001) != 0;
                target->status.discharging     = (present_status & 0x0002) != 0;
                target->status.online          = (present_status & 0x0004) != 0;  // ACPresent
                bool battery_present           = (present_status & 0x0008) != 0;
                target->status.low_battery     = (present_status & 0x0010) != 0;
                bool shutdown_imminent         = (present_status & 0x0020) != 0;
                bool time_limit_expired        = (present_status & 0x0040) != 0;
                target->status.replace_battery = (present_status & 0x0100) != 0;
                target->status.overload        = (present_status & 0x0200) != 0;

                // Boost/Trim are not carried in this report on this device; leave
                // them alone rather than inventing them from unrelated bits.

                ESP_LOGI(TAG, "   ├─ Raw: 0x%04X", present_status);
                ESP_LOGI(TAG, "   ├─ BatteryPresent=%d ShutdownImminent=%d TimeLimitExpired=%d",
                         battery_present, shutdown_imminent, time_limit_expired);
                ESP_LOGI(TAG, "   └─ Status: [%s%s%s%s%s%s]",
                         target->status.online ? "OL " : "",
                         target->status.discharging ? "DISCHRG " : "",
                         target->status.charging ? "CHRG " : "",
                         target->status.low_battery ? "LB " : "",
                         target->status.overload ? "OVER " : "",
                         target->status.replace_battery ? "RB" : "");
                updated = true;
            }
            break;

        case 0x17:  // PowerSummary.RemainingTimeLimit (NOT a reboot timer)
            ESP_LOGI(TAG, "   Type: Remaining Time Limit");
            if (length >= 3) {
                // Descriptor: Battery page usage 0x2A (RemainingTimeLimit) with a
                // seconds unit, inside the PowerSummary collection — the same
                // quantity report 0x24 carries for UPS.Battery. It is NOT a reboot
                // timer; this device exposes no DelayBeforeReboot usage at all, so
                // reboot_timer is left unset and main.c stops publishing it.
                uint16_t seconds = data[1] | (data[2] << 8);
                target->low_battery_runtime_threshold = (float)seconds;
                ESP_LOGI(TAG, "   └─ RemainingTimeLimit: %ds", seconds);
                updated = true;
            }
            break;

        case 0x18:  // AudibleAlarmControl — the beeper (Power page 0x5A)
            // Descriptor: logical range 1..3. This report was previously decoded
            // as the self-test result, which is why self_test_result read
            // "Test passed" on a UPS that had never run a test.
            ESP_LOGI(TAG, "   Type: Beeper Status (AudibleAlarmControl)");
            if (length >= 2) {
                const char *beeper_state[] = {"disabled", "enabled", "muted"};
                uint8_t beeper_val = data[1];
                if (beeper_val >= 1 && beeper_val <= 3) {
                    strncpy(target->beeper_status, beeper_state[beeper_val - 1],
                            sizeof(target->beeper_status) - 1);
                    ESP_LOGI(TAG, "   └─ Beeper: %s", target->beeper_status);
                    updated = true;
                } else {
                    ESP_LOGW(TAG, "   └─ Beeper: unexpected value %d", beeper_val);
                }
            }
            break;

        case 0x21:  // Test — the real self-test result (Power page 0x58)
            ESP_LOGI(TAG, "   Type: Self-Test Result");
            if (length >= 2) {
                const char *test_results[] = {
                    "Done and passed",
                    "Done and warning",
                    "Done and error",
                    "Aborted",
                    "In progress",
                    "No test initiated",
                    "Test scheduled"
                };
                uint8_t test_val = data[1];
                if (test_val >= 1 && test_val <= 7) {
                    strncpy(target->self_test_result, test_results[test_val - 1],
                            sizeof(target->self_test_result) - 1);
                    ESP_LOGI(TAG, "   └─ Result: %s", target->self_test_result);
                } else {
                    snprintf(target->self_test_result, sizeof(target->self_test_result),
                             "Unknown (%d)", test_val);
                    ESP_LOGW(TAG, "   └─ Result: unexpected value %d", test_val);
                }
                updated = true;
            }
            break;

        case 0x1C:  // Vendor page 0xFF86 usage 0x16, 24-bit — meaning unknown
            // Previously decoded as a battery manufacture date, which it is not.
            // Log only, so it cannot clobber the real date from report 0x20.
            ESP_LOGI(TAG, "   Type: Vendor 0xFF86:0x16 (unidentified)");
            if (length >= 4) {
                ESP_LOGI(TAG, "   └─ Raw: 0x%02X%02X%02X (not published)",
                         data[3], data[2], data[1]);
            }
            break;

        case 0x20:  // UPS.Battery.ManufacturerDate (Battery page usage 0x85)
            ESP_LOGI(TAG, "   Type: Battery Manufacture Date");
            if (length >= 3) {
                uint16_t packed = data[1] | (data[2] << 8);
                format_hid_date(target->battery_mfr_date,
                                sizeof(target->battery_mfr_date), packed);
                ESP_LOGI(TAG, "   └─ Date: %s (packed 0x%04X)",
                         target->battery_mfr_date, packed);
                updated = true;
            }
            break;

        case 0x13:  // ACPresent as a byte (Battery page 0xD0)
            // Not a reboot delay — that is report 0x40 (APCDelayBeforeReboot).
            // This used to publish delay_reboot=1, which was really "AC is on".
            ESP_LOGI(TAG, "   Type: AC Present");
            if (length >= 2) {
                target->status.online = (data[1] != 0);
                ESP_LOGI(TAG, "   └─ ACPresent: %d", target->status.online);
                updated = true;
            }
            break;

        case 0x14:  // BelowRemainingCapacityLimit + ShutdownImminent, as bytes
            // Descriptor: Battery 0x42 then Power 0x69, one byte each. This is a
            // second, independent source for the low-battery flag — it was the
            // cross-check that proved the old LB was bogus (it reads 0 while the
            // old decode of 0x16 asserted LB). Not a shutdown delay.
            ESP_LOGI(TAG, "   Type: Low Battery / Shutdown Imminent");
            if (length >= 3) {
                target->status.low_battery = (data[1] != 0);
                ESP_LOGI(TAG, "   ├─ BelowRemainingCapacityLimit: %d", data[1]);
                ESP_LOGI(TAG, "   └─ ShutdownImminent: %d", data[2]);
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

        /* Report 0x21 used to be decoded here as "last transfer reason". The
         * descriptor says otherwise (Power page 0x58 Test, logical max 6), so it
         * is handled above as the self-test result. APC's line-fail cause lives
         * on a vendor page we have not located yet — last_transfer_reason is
         * therefore left unset, and main.c skips publishing empty strings. */

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

        case 0x36:  // APCLineFailCause (vendor 0xFF86:0x52) — last transfer reason
            // Named from NUT's apc_usage_lkp. This is NOT input frequency; this
            // UPS reports no frequency usage anywhere in its descriptor. The
            // reason table below used to be applied to report 0x21, which is
            // actually the self-test result.
            ESP_LOGI(TAG, "   Type: Line Fail Cause (last transfer reason)");
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
                if (reason < (sizeof(reasons) / sizeof(reasons[0]))) {
                    strlcpy(target->last_transfer_reason, reasons[reason],
                            sizeof(target->last_transfer_reason));
                } else {
                    snprintf(target->last_transfer_reason,
                             sizeof(target->last_transfer_reason),
                             "Unknown (%d)", reason);
                }
                ESP_LOGI(TAG, "   └─ Cause %d: %s", reason, target->last_transfer_reason);
                updated = true;
            }
            break;

        case 0x40:  // APCDelayBeforeReboot (vendor 0xFF86:0x7C), 8-bit
            ESP_LOGI(TAG, "   Type: Delay Before Reboot");
            if (length >= 2) {
                target->delay_before_reboot = (float)data[1];
                ESP_LOGI(TAG, "   └─ Delay: %.0fs", target->delay_before_reboot);
                updated = true;
            }
            break;

        case 0x41:  // APCDelayBeforeShutdown (vendor 0xFF86:0x7D), 16-bit
            ESP_LOGI(TAG, "   Type: Delay Before Shutdown");
            if (length >= 3) {
                int16_t delay = (int16_t)(data[1] | (data[2] << 8));
                target->delay_before_shutdown = (float)delay;
                ESP_LOGI(TAG, "   └─ Delay: %.0fs", target->delay_before_shutdown);
                updated = true;
            }
            break;

        case 0x03:  // iDeviceChemistry — a STRING INDEX, not a chemistry code
            // The descriptor declares this as Battery page 0x89 with String
            // Index 4. The old code fed the index into a chemistry enum, where
            // 4 landed on "NiMH" — hence a lead-acid APC reporting NiMH. The
            // actual text comes from string descriptor 4 (see apc_hid_set_string).
            ESP_LOGI(TAG, "   Type: Battery Chemistry (string index)");
            if (length >= 2) {
                ESP_LOGI(TAG, "   └─ String index %d (text fetched separately)", data[1]);
            }
            break;

        case 0x07:  // UPS manufacture date (or unknown field)
            ESP_LOGI(TAG, "   Type: UPS Manufacture Date");
            // NOTE: This report returns only 3 bytes: 07 D6 54
            // Interpreting as date gives nonsensical year 21718
            // Likely this is NOT a date field or uses different encoding
            // Same data as Report 0x20 - both appear to be unidentified fields
            if (length >= 3) {
                // Same packed-date encoding as report 0x20, but for the UPS
                // itself (PowerSummary.ManufacturerDate). No field to store it in,
                // so log it decoded rather than claiming it is unparseable.
                char decoded[16];
                uint16_t packed = data[1] | (data[2] << 8);
                format_hid_date(decoded, sizeof(decoded), packed);
                ESP_LOGI(TAG, "   └─ Date: %s (packed 0x%04X, not published)",
                         decoded, packed);
            }
            break;

        case 0x34:  // Vendor 0xFF86:0x24, 16-bit — not in NUT's usage table
            // Logical range 117..139 suggests a voltage setpoint, but it is not
            // the input sensitivity (that is report 0x35, APCSensitivity).
            ESP_LOGI(TAG, "   Type: Vendor 0xFF86:0x24 (unidentified)");
            if (length >= 3) {
                ESP_LOGI(TAG, "   └─ Value: %d (not published)", data[1] | (data[2] << 8));
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
