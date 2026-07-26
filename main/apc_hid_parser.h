#ifndef APC_HID_PARSER_H
#define APC_HID_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool online;
    bool discharging;
    bool charging;
    bool low_battery;
    bool overload;
    bool replace_battery;
    bool boost;
    bool trim;
} ups_status_t;

typedef struct {
    // Battery metrics
    float battery_charge;              // %
    float battery_voltage;             // V
    float battery_runtime;             // seconds
    float battery_nominal_voltage;     // V
    float battery_warning_threshold;   // %
    char battery_type[16];             // e.g., "PbAc"
    char battery_mfr_date[16];         // YYYY/MM/DD

    // Input/Output metrics
    float input_voltage;               // V
    float input_voltage_nominal;       // V
    float input_frequency;             // Hz
    float output_voltage;              // V
    float load_percent;                // %
    float nominal_power;               // W

    // Transfer points
    float high_voltage_transfer;       // V
    float low_voltage_transfer;        // V
    char input_sensitivity[16];        // low/medium/high
    char last_transfer_reason[64];

    // Battery thresholds
    float low_battery_charge_threshold;   // %
    float low_battery_runtime_threshold;  // seconds

    // Timers
    float shutdown_delay;              // seconds
    float shutdown_timer;              // seconds
    float reboot_timer;                // seconds
    float delay_before_reboot;         // seconds
    float delay_before_shutdown;       // seconds

    // Device info
    char firmware_version[32];
    char driver_name[32];
    char driver_state[16];
    char driver_version[16];
    char beeper_status[16];            // enabled/disabled/muted
    char self_test_result[64];
    char power_failure_status[16];     // OK or reason

    // Status
    ups_status_t status;
    char status_string[64];

    uint32_t last_update_ms;
    bool valid;
} ups_metrics_t;

void apc_hid_parser_init(void);
bool apc_hid_parse_report(uint8_t report_id, const uint8_t *data, size_t length, ups_metrics_t *metrics);
const ups_metrics_t* apc_hid_get_metrics(void);
void apc_hid_format_status(const ups_status_t *status, char *buffer, size_t buffer_size);

#endif // APC_HID_PARSER_H
