#include "ups_map.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* Collections we disambiguate on. Voltage (0x84:0x30) alone is meaningless:
 * it is battery voltage under PowerSummary, mains voltage under Input, and
 * output voltage under Output. NUT carries duplicate rows for this same reason. */
#define C_UPS            0x84, 0x04
#define C_BATTERY_SYS    0x84, 0x10
#define C_BATTERY        0x84, 0x12
#define C_POWER_CONV     0x84, 0x16
#define C_OUTLET_SYS     0x84, 0x18
#define C_INPUT          0x84, 0x1A
#define C_OUTPUT         0x84, 0x1C
#define C_OUTLET         0x84, 0x20
#define C_POWER_SUMMARY  0x84, 0x24
#define C_PRESENT_STATUS 0x84, 0x02
#define C_ANY            0x00, 0x00

typedef enum {
    K_FLOAT,       /* scaled by unit exponent into a float member */
    K_BOOL,        /* nonzero into a bool member of ups_status_t */
    K_STRING_IDX,  /* value is a USB string descriptor index */
    K_DATE,        /* HID packed date into a char[] member */
    K_BEEPER,      /* AudibleAlarmControl 1..3 */
    K_TEST,        /* Test result code */
} kind_t;

typedef struct {
    uint16_t    a_page, a_id;   /* required ancestor collection; 0,0 = any */
    uint16_t    l_page, l_id;   /* leaf usage */
    uint8_t     kind;
    uint16_t    offset;         /* offsetof into ups_metrics_t */
    uint8_t     size;           /* sizeof that member; char[] writes clamp to it */
    const char *name;
} entry_t;

/* Carry the member's real size alongside its offset. The char[] members are not
 * all the same width (ups_model[40] vs battery_type[16]), so a fixed clamp
 * would silently truncate model and manufacturer strings. */
#define M(field) offsetof(ups_metrics_t, field), \
                 (uint8_t)sizeof(((ups_metrics_t *)0)->field)

/*
 * Ordered specific-first: an entry with an ancestor must be tried before the
 * catch-all for the same leaf usage, or Input.Voltage would land in whichever
 * field the loose entry names.
 */
static const entry_t kMap[] = {
    /* ── Battery ─────────────────────────────────────────────────────── */
    { C_ANY,           0x85, 0x66, K_FLOAT, M(battery_charge),                "battery_charge" },
    { C_ANY,           0x85, 0x68, K_FLOAT, M(battery_runtime),               "battery_runtime" },
    { C_ANY,           0x85, 0x29, K_FLOAT, M(low_battery_charge_threshold),  "battery_charge_low" },
    { C_ANY,           0x85, 0x2A, K_FLOAT, M(low_battery_runtime_threshold), "battery_runtime_low" },
    { C_ANY,           0x85, 0x8C, K_FLOAT, M(battery_warning_threshold),     "battery_charge_warning" },
    { C_ANY,           0x85, 0x85, K_DATE,  M(battery_mfr_date),              "battery_mfr_date" },

    /* Battery voltage. PowerSummary.Voltage is the summary (battery) voltage on
     * a UPS, which is how the deployed APC reports it (report 0x09). */
    { C_POWER_SUMMARY, 0x84, 0x30, K_FLOAT, M(battery_voltage),         "battery_voltage" },
    { C_BATTERY,       0x84, 0x30, K_FLOAT, M(battery_voltage),         "battery_voltage" },
    { C_BATTERY_SYS,   0x84, 0x30, K_FLOAT, M(battery_voltage),         "battery_voltage" },
    { C_POWER_SUMMARY, 0x84, 0x40, K_FLOAT, M(battery_nominal_voltage), "battery_voltage_nominal" },
    { C_BATTERY,       0x84, 0x40, K_FLOAT, M(battery_nominal_voltage), "battery_voltage_nominal" },
    { C_BATTERY_SYS,   0x84, 0x40, K_FLOAT, M(battery_nominal_voltage), "battery_voltage_nominal" },

    /* ── Input ───────────────────────────────────────────────────────── */
    { C_INPUT,         0x84, 0x30, K_FLOAT, M(input_voltage),         "input_voltage" },
    { C_INPUT,         0x84, 0x40, K_FLOAT, M(input_voltage_nominal), "input_voltage_nominal" },
    { C_INPUT,         0x84, 0x32, K_FLOAT, M(input_frequency),       "input_frequency" },

    /* ── Output / load ───────────────────────────────────────────────── */
    { C_OUTPUT,        0x84, 0x30, K_FLOAT, M(output_voltage), "output_voltage" },
    { C_OUTPUT,        0x84, 0x35, K_FLOAT, M(load_percent),   "load_percent" },
    { C_POWER_CONV,    0x84, 0x35, K_FLOAT, M(load_percent),   "load_percent" },
    { C_OUTLET_SYS,    0x84, 0x35, K_FLOAT, M(load_percent),   "load_percent" },
    { C_OUTLET,        0x84, 0x35, K_FLOAT, M(load_percent),   "load_percent" },
    { C_ANY,           0x84, 0x35, K_FLOAT, M(load_percent),   "load_percent" },
    { C_OUTPUT,        0x84, 0x44, K_FLOAT, M(nominal_power),  "nominal_power" },
    { C_ANY,           0x84, 0x44, K_FLOAT, M(nominal_power),  "nominal_power" },

    /* ── Transfer points ─────────────────────────────────────────────── */
    { C_ANY,           0x84, 0x53, K_FLOAT, M(low_voltage_transfer),  "input_transfer_low" },
    { C_ANY,           0x84, 0x54, K_FLOAT, M(high_voltage_transfer), "input_transfer_high" },

    /* ── Timers ──────────────────────────────────────────────────────── */
    { C_ANY,           0x84, 0x55, K_FLOAT, M(delay_before_reboot),   "delay_reboot" },
    { C_ANY,           0x84, 0x57, K_FLOAT, M(delay_before_shutdown), "delay_shutdown" },

    /* ── Controls / results ──────────────────────────────────────────── */
    { C_ANY,           0x84, 0x5A, K_BEEPER, M(beeper_status),    "beeper_status" },
    { C_ANY,           0x84, 0x58, K_TEST,   M(self_test_result), "self_test_result" },

    /* ── Identity. Values are string descriptor INDICES, not text. ───── */
    { C_ANY,           0x84, 0xFD, K_STRING_IDX, M(ups_manufacturer), "ups_manufacturer" },
    { C_ANY,           0x85, 0x8F, K_STRING_IDX, M(ups_manufacturer), "ups_manufacturer" },
    { C_ANY,           0x85, 0x87, K_STRING_IDX, M(ups_manufacturer), "ups_manufacturer" },
    { C_ANY,           0x84, 0xFE, K_STRING_IDX, M(ups_model),        "ups_model" },
    { C_ANY,           0x85, 0x88, K_STRING_IDX, M(ups_model),        "ups_model" },
    { C_ANY,           0x84, 0xFF, K_STRING_IDX, M(ups_serial),       "ups_serial" },
    { C_ANY,           0x85, 0x86, K_STRING_IDX, M(ups_serial),       "ups_serial" },
    { C_ANY,           0x85, 0x89, K_STRING_IDX, M(battery_type),     "battery_type" },

    /* ── PresentStatus flags ─────────────────────────────────────────── */
    { C_ANY,           0x85, 0x44, K_BOOL, M(status.charging),        "charging" },
    { C_ANY,           0x85, 0x45, K_BOOL, M(status.discharging),     "discharging" },
    { C_ANY,           0x85, 0xD0, K_BOOL, M(status.online),          "online" },
    { C_ANY,           0x85, 0x42, K_BOOL, M(status.low_battery),     "low_battery" },
    { C_ANY,           0x85, 0x4B, K_BOOL, M(status.replace_battery), "replace_battery" },
    { C_ANY,           0x84, 0x65, K_BOOL, M(status.overload),        "overload" },
    { C_ANY,           0x84, 0x6E, K_BOOL, M(status.boost),           "boost" },
    { C_ANY,           0x84, 0x6F, K_BOOL, M(status.trim),            "trim" },

    { C_ANY, 0, 0, 0, 0, 0, NULL }
};

/* Does this field sit inside the named collection? */
static bool has_ancestor(const hid_pdc_field_t *f, uint16_t page, uint16_t id)
{
    int keep = (f->path_len < HID_PDC_MAX_PATH) ? f->path_len : HID_PDC_MAX_PATH;
    for (int c = 0; c < keep; c++) {
        if (f->path[c].page == page && f->path[c].id == id) return true;
    }
    return false;
}

static const entry_t *lookup(const hid_pdc_field_t *f)
{
    for (int i = 0; kMap[i].name != NULL; i++) {
        const entry_t *e = &kMap[i];
        if (e->l_page != f->usage.page || e->l_id != f->usage.id) continue;
        if (e->a_page != 0 && !has_ancestor(f, e->a_page, e->a_id)) continue;
        return e;
    }
    return NULL;
}

const char *ups_map_field_name(const hid_pdc_field_t *f)
{
    if (f == NULL) return NULL;
    const entry_t *e = lookup(f);
    return (e != NULL) ? e->name : NULL;
}

/* HID packed date: bits 15..9 year-1980, 8..5 month, 4..0 day. Printing the raw
 * word is how battery_mfr_date once read "21690 days" (apc_hid_parser.c:114). */
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

int ups_map_apply(const hid_pdc_map_t *map,
                  uint8_t report_id, uint8_t report_type,
                  const uint8_t *payload, size_t payload_len,
                  ups_metrics_t *out,
                  ups_map_get_string_fn get_string, void *ctx)
{
    if (map == NULL || payload == NULL || out == NULL) return 0;

    int applied = 0;

    for (int i = 0; i < map->count; i++) {
        const hid_pdc_field_t *f = &map->fields[i];
        if (f->report_id != report_id) continue;
        if (f->report_type != report_type) continue;

        const entry_t *e = lookup(f);
        if (e == NULL) continue;

        int32_t raw;
        if (!hid_pdc_extract(f, payload, payload_len, &raw)) continue;

        uint8_t *base = (uint8_t *)out + e->offset;

        switch (e->kind) {
        case K_FLOAT:
            *(float *)base = hid_pdc_scale(f, raw);
            applied++;
            break;

        case K_BOOL:
            *(bool *)base = (raw != 0);
            applied++;
            break;

        case K_DATE:
            format_hid_date((char *)base, e->size, (uint16_t)raw);
            applied++;
            break;

        case K_BEEPER: {
            /* Logical range 1..3 per the descriptor. */
            static const char *kBeeper[] = { "disabled", "enabled", "muted" };
            if (raw >= 1 && raw <= 3) {
                snprintf((char *)base, e->size, "%s", kBeeper[raw - 1]);
                applied++;
            }
            break;
        }

        case K_TEST: {
            static const char *kTest[] = {
                "Done and passed", "Done and warning", "Done and error",
                "Aborted", "In progress", "No test initiated",
            };
            if (raw >= 1 && raw <= 6) {
                snprintf((char *)base, e->size, "%s", kTest[raw - 1]);
                applied++;
            }
            break;
        }

        case K_STRING_IDX: {
            /* raw is a USB string descriptor index. Index 0 means "absent". */
            if (get_string == NULL || raw <= 0 || raw > 255) break;
            char buf[64];
            if (get_string((uint8_t)raw, buf, sizeof(buf), ctx) && buf[0] != '\0') {
                snprintf((char *)base, e->size, "%s", buf);
                applied++;
            }
            break;
        }

        default:
            break;
        }
    }

    if (applied > 0) out->valid = true;
    return applied;
}

int ups_map_feature_report_ids(const hid_pdc_map_t *map, uint8_t *out, int max_out)
{
    if (map == NULL || out == NULL || max_out <= 0) return 0;

    int n = 0;
    for (int i = 0; i < map->count; i++) {
        const hid_pdc_field_t *f = &map->fields[i];
        if (f->report_type != HID_PDC_FEATURE) continue;
        if (lookup(f) == NULL) continue;

        bool seen = false;
        for (int k = 0; k < n; k++) {
            if (out[k] == f->report_id) { seen = true; break; }
        }
        if (seen) continue;

        if (n >= max_out) break;
        out[n++] = f->report_id;
    }
    return n;
}

bool ups_map_is_usable(const hid_pdc_map_t *map)
{
    if (map == NULL || !map->has_power_page) return false;

    /* One mapped field is not enough to call something a UPS; require at least
     * a charge reading or a status flag, which every PDC implementation has. */
    bool has_charge = false, has_status = false;
    for (int i = 0; i < map->count; i++) {
        const hid_pdc_field_t *f = &map->fields[i];
        if (f->usage.page == 0x85 && f->usage.id == 0x66) has_charge = true;
        if (f->usage.page == 0x85 && (f->usage.id == 0xD0 || f->usage.id == 0x45)) has_status = true;
    }
    return has_charge || has_status;
}
