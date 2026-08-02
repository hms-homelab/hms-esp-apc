#ifndef HID_PDC_H
#define HID_PDC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * HID report descriptor walker.
 *
 * WHY THIS EXISTS
 * ---------------
 * apc_hid_parser.c decodes reports with byte offsets and bit positions that
 * were reverse-engineered from one specific APC's report descriptor. Report IDs
 * are assigned arbitrarily by each vendor's firmware, so "report 0x16 is
 * PresentStatus" is a fact about that one APC and nothing more. Every new UPS
 * would be a fresh reverse-engineering job, and the guessed offsets have been a
 * recurring source of wrong readings (see the v1.14.1 changelog: LB asserted at
 * 97% charge, beeper reported as self-test result).
 *
 * The device tells us the real layout at enumeration. usb_host_manager already
 * fetches the report descriptor and hid_debug already stores it; we were just
 * throwing the information away. This module parses it into a field table so
 * decode can be driven by HID usage paths instead of hardcoded offsets.
 *
 * Deliberately has NO ESP-IDF dependencies (no esp_log, no FreeRTOS) so the
 * same source compiles natively for the host-side test harness. Callers do the
 * logging.
 *
 * Reference: USB Device Class Definition for HID 1.11, section 6.2.2.
 */

#define HID_PDC_MAX_FIELDS   224  /* fields kept from one descriptor */
#define HID_PDC_MAX_PATH       6  /* collection nesting recorded per field */
#define HID_PDC_MAX_COLL       8  /* collection stack depth while parsing */
#define HID_PDC_MAX_USAGES    32  /* pending local usages per main item */

/* Usage Pages we care about. A device is a UPS if it declares 0x84. */
#define HID_PAGE_POWER_DEVICE  0x84
#define HID_PAGE_BATTERY       0x85

typedef struct {
    uint16_t page;
    uint16_t id;
} hid_usage_t;

typedef enum {
    HID_PDC_INPUT   = 0,
    HID_PDC_OUTPUT  = 1,
    HID_PDC_FEATURE = 2,
} hid_pdc_report_type_t;

/* Main item data bits (HID 1.11 section 6.2.2.5). */
#define HID_PDC_F_CONSTANT  (1u << 0)  /* padding; carries no data */
#define HID_PDC_F_VARIABLE  (1u << 1)  /* 0 = array, 1 = variable */
#define HID_PDC_F_RELATIVE  (1u << 2)

typedef struct {
    uint8_t     report_id;
    uint8_t     report_type;   /* hid_pdc_report_type_t */
    hid_usage_t usage;         /* the leaf usage of this field */

    /* Enclosing collections, outermost first. Needed because the same leaf
     * usage appears in several places: Voltage (0x84:0x30) shows up under
     * PowerSummary, Input, Output and BatterySystem, and they mean different
     * things. NUT carries duplicate mapping rows for exactly this reason. */
    hid_usage_t path[HID_PDC_MAX_PATH];
    uint8_t     path_len;      /* may exceed MAX_PATH; entries beyond are dropped */

    /* Bit position within the report PAYLOAD, i.e. AFTER the report ID byte.
     * A report arriving as {0x16, 0x0D, 0x00} has its first field at
     * bit_offset 0, which is bit 0 of data[1]. */
    uint16_t    bit_offset;
    uint8_t     bit_size;

    int32_t     logical_min;
    int32_t     logical_max;
    int8_t      unit_exponent; /* signed; value is raw * 10^unit_exponent */
    uint32_t    unit;
    uint8_t     flags;         /* HID_PDC_F_* */
} hid_pdc_field_t;

typedef struct {
    hid_pdc_field_t fields[HID_PDC_MAX_FIELDS];
    int             count;
    bool            truncated;       /* descriptor had more fields than we kept */
    bool            has_power_page;  /* declared Usage Page 0x84 anywhere */
    bool            uses_report_ids; /* descriptor contained a Report ID item */
} hid_pdc_map_t;

/*
 * Parse a raw HID report descriptor into a field table.
 * Returns true if the descriptor was walked to the end without a structural
 * error. A truncated table (too many fields) still returns true; check
 * map->truncated. Malformed input returns false and map holds whatever was
 * parsed before the error.
 */
bool hid_pdc_parse(const uint8_t *desc, size_t desc_len, hid_pdc_map_t *map);

/*
 * Pull one field's raw value out of a report payload.
 *
 * `payload` must point at the byte AFTER the report ID, and payload_len is the
 * length from there. Returns false if the field runs past the end of the data,
 * which happens routinely: a device may send a short report, and reading past
 * it is how you get garbage that looks like a plausible reading.
 *
 * Sign extension follows the rule Linux hid-core uses: the value is signed only
 * when logical_min < 0. Encoding Logical Maximum 255 as 0xFF is extremely
 * common and would otherwise read as -1.
 */
bool hid_pdc_extract(const hid_pdc_field_t *f, const uint8_t *payload,
                     size_t payload_len, int32_t *out_raw);

/*
 * Convert a raw value to its real-world quantity, replacing the scattered magic
 * divisors in apc_hid_parser.c (/100.0f, /10.0f) with the scaling the device
 * itself declares.
 *
 * Applies BOTH the declared Unit Exponent and the Unit field's dimensional
 * scale. The second part is not optional: HID's SI Linear system is
 * centimetre-gram-second, so a volt in HID units is 10^-7 V. A real APC
 * declares Unit Exponent 5 on its battery voltage and expects the reader to
 * add -7, which is how 1371 becomes 13.71 V rather than 137100000 V.
 */
float hid_pdc_scale(const hid_pdc_field_t *f, int32_t raw);

/* The decimal exponent correction implied by a Unit field's dimensions.
 * Exposed for tests; hid_pdc_scale applies it already. */
int hid_pdc_unit_scale_exp(uint32_t unit);

/*
 * Find a field by leaf usage, optionally requiring an ancestor collection.
 * Pass ancestor_page = 0 to match on the leaf usage alone.
 * Returns NULL when absent, which is the normal answer for any given metric on
 * any given UPS.
 */
const hid_pdc_field_t *hid_pdc_find(const hid_pdc_map_t *map,
                                    uint8_t report_type,
                                    uint16_t ancestor_page, uint16_t ancestor_id,
                                    uint16_t leaf_page, uint16_t leaf_id);

/* Human-readable usage path, e.g. "UPS.PowerSummary.PresentStatus.ACPresent".
 * Falls back to hex for usages with no known name. */
void hid_pdc_format_path(const hid_pdc_field_t *f, char *out, size_t out_size);

/* Name for a single usage, or NULL if unknown. */
const char *hid_pdc_usage_name(uint16_t page, uint16_t id);

#endif /* HID_PDC_H */
