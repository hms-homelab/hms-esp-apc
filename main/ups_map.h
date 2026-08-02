#ifndef UPS_MAP_H
#define UPS_MAP_H

#include "hid_pdc.h"
#include "apc_hid_parser.h"   /* for ups_metrics_t; moves to its own header
                               * once apc_hid_parser.c is retired */

/*
 * Maps HID usage paths onto ups_metrics_t.
 *
 * This is the layer that makes a new UPS cost nothing: hid_pdc.c says where
 * each field lives in each report, and this table says what each field means.
 * Neither is device specific, so any UPS that implements the HID Power Device
 * Class correctly is decoded without new code.
 *
 * Equivalent to NUT's hid2nut tables, minus the NUT variable names.
 */

/*
 * Fetches USB string descriptor `index` into `out`. Supplied by the caller
 * because it needs a USB control transfer, which this module deliberately knows
 * nothing about. Return false if unavailable.
 *
 * This is how manufacturer/model/serial/chemistry are read: the report carries
 * only the string INDEX, never the text. Decoding that index as if it were an
 * enum is what produced "NiMH" on a lead-acid UPS (apc_hid_parser.c:83).
 */
typedef bool (*ups_map_get_string_fn)(uint8_t index, char *out, size_t out_size, void *ctx);

/*
 * Decode every mapped field of one report into `out`.
 *
 * `payload` points AFTER the report ID byte. `get_string` may be NULL, in which
 * case string-index fields are skipped. Returns the number of fields applied.
 */
int ups_map_apply(const hid_pdc_map_t *map,
                  uint8_t report_id, uint8_t report_type,
                  const uint8_t *payload, size_t payload_len,
                  ups_metrics_t *out,
                  ups_map_get_string_fn get_string, void *ctx);

/*
 * Collect the distinct Feature report IDs that carry at least one mapped field.
 * This replaces the hardcoded poll_reports[] list in usb_host_manager.c: we poll
 * what this device actually offers and we care about, rather than a list
 * transcribed from one APC's NUT exploration.
 *
 * Returns the count written to `out`.
 */
int ups_map_feature_report_ids(const hid_pdc_map_t *map, uint8_t *out, int max_out);

/* True if this descriptor exposes enough to be worth treating as a UPS. */
bool ups_map_is_usable(const hid_pdc_map_t *map);

/* Name this module would give the field, or NULL if it maps nothing.
 * Used by /hid so the shadow decode is readable. */
const char *ups_map_field_name(const hid_pdc_field_t *f);

#endif /* UPS_MAP_H */
