/*
 * Replay a real device's HID report descriptor and raw reports through
 * hid_pdc + ups_map on the host.
 *
 *   python3 extract_hid.py hid_capture.txt cap/
 *   cc -Wall -Wextra -I../main -o /tmp/replay hid_replay.c ../main/hid_pdc.c ../main/ups_map.c
 *   /tmp/replay cap/descriptor.bin cap/reports.txt
 *
 * Prints the parsed field table and the decoded metrics, so they can be diffed
 * against the DECODED section the same board printed with the hand-written
 * parser. This is the comparison that gates moving off shadow mode, and it runs
 * without flashing anything.
 */

#include "hid_pdc.h"
#include "ups_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hid_pdc_map_t map;
static ups_metrics_t metrics;

static size_t load_file(const char *path, uint8_t *buf, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return n;
}

/* Stand-in for the USB string descriptor fetch. The capture does not carry the
 * string table, so identity fields are reported as the index the device named. */
static bool note_string_index(uint8_t index, char *out, size_t out_size, void *ctx)
{
    (void)ctx;
    snprintf(out, out_size, "<string idx %u>", index);
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <descriptor.bin> <reports.txt>\n", argv[0]);
        return 2;
    }

    static uint8_t desc[8192];
    size_t desc_len = load_file(argv[1], desc, sizeof(desc));
    printf("descriptor: %zu bytes\n", desc_len);

    if (!hid_pdc_parse(desc, desc_len, &map)) {
        printf("PARSE FAILED\n");
        return 1;
    }
    printf("parsed %d fields%s | power page: %s | report ids: %s\n\n",
           map.count,
           map.truncated ? " (TRUNCATED)" : "",
           map.has_power_page ? "yes" : "NO",
           map.uses_report_ids ? "yes" : "no");

    printf("== FIELDS ==\n");
    printf(" id  type  bit  sz  exp  usage path -> mapped\n");
    for (int i = 0; i < map.count; i++) {
        const hid_pdc_field_t *f = &map.fields[i];
        char path[160];
        hid_pdc_format_path(f, path, sizeof(path));
        const char *mapped = ups_map_field_name(f);
        printf(" %02X  %-4s  %3u  %2u  %3d  %s%s%s\n",
               f->report_id,
               f->report_type == HID_PDC_FEATURE ? "FEAT" :
               f->report_type == HID_PDC_INPUT   ? "IN"   : "OUT",
               f->bit_offset, f->bit_size, f->unit_exponent,
               path, mapped ? "  -> " : "", mapped ? mapped : "");
    }

    /* Derived poll list, replacing the hardcoded one. */
    uint8_t ids[64];
    int nids = ups_map_feature_report_ids(&map, ids, 64);
    printf("\n== DERIVED FEATURE POLL LIST (%d) ==\n ", nids);
    for (int i = 0; i < nids; i++) printf("%02X ", ids[i]);
    printf("\n");
    printf("usable as a UPS: %s\n", ups_map_is_usable(&map) ? "yes" : "NO");

    /* Replay the captured reports. */
    memset(&metrics, 0, sizeof(metrics));
    FILE *rf = fopen(argv[2], "r");
    if (!rf) { perror(argv[2]); return 1; }

    char line[1024];
    int replayed = 0, applied_total = 0;
    printf("\n== REPLAY ==\n");
    while (fgets(line, sizeof(line), rf)) {
        char type[8];
        unsigned rid;
        int pos = 0;
        if (sscanf(line, "%7s %X %n", type, &rid, &pos) < 2) continue;

        uint8_t payload[64];
        size_t n = 0;
        const char *p = line + pos;
        while (n < sizeof(payload)) {
            unsigned b;
            int used = 0;
            if (sscanf(p, "%2X%n", &b, &used) != 1 || used == 0) break;
            payload[n++] = (uint8_t)b;
            p += used;
            while (*p == ' ') p++;
        }

        uint8_t rtype = (strcmp(type, "FEAT") == 0) ? HID_PDC_FEATURE : HID_PDC_INPUT;
        int applied = ups_map_apply(&map, (uint8_t)rid, rtype, payload, n,
                                    &metrics, note_string_index, NULL);
        if (applied > 0) {
            printf("  %s %02X  %zu bytes -> %d field(s)\n", type, rid, n, applied);
            applied_total += applied;
        }
        replayed++;
    }
    fclose(rf);
    printf("  replayed %d reports, %d fields applied\n", replayed, applied_total);

    printf("\n== DECODED (compare against the board's own DECODED section) ==\n");
    printf("  battery_charge          = %.1f %%\n", (double)metrics.battery_charge);
    printf("  battery_runtime         = %.0f s\n",  (double)metrics.battery_runtime);
    printf("  battery_voltage         = %.2f V\n",  (double)metrics.battery_voltage);
    printf("  battery_nominal_voltage = %.2f V\n",  (double)metrics.battery_nominal_voltage);
    printf("  input_voltage           = %.1f V\n",  (double)metrics.input_voltage);
    printf("  input_voltage_nominal   = %.1f V\n",  (double)metrics.input_voltage_nominal);
    printf("  input_frequency         = %.1f Hz\n", (double)metrics.input_frequency);
    printf("  load_percent            = %.1f %%\n", (double)metrics.load_percent);
    printf("  nominal_power           = %.1f W\n",  (double)metrics.nominal_power);
    printf("  low_charge_threshold    = %.0f %%\n", (double)metrics.low_battery_charge_threshold);
    printf("  low_runtime_threshold   = %.0f s\n",  (double)metrics.low_battery_runtime_threshold);
    printf("  transfer low/high       = %.0f / %.0f V\n",
           (double)metrics.low_voltage_transfer, (double)metrics.high_voltage_transfer);
    printf("  beeper                  = %s\n", metrics.beeper_status);
    printf("  self_test               = %s\n", metrics.self_test_result);
    printf("  battery_mfr_date        = %s\n", metrics.battery_mfr_date);
    printf("  flags: OL=%d DISCHRG=%d CHRG=%d LB=%d OVER=%d RB=%d BOOST=%d TRIM=%d\n",
           metrics.status.online, metrics.status.discharging, metrics.status.charging,
           metrics.status.low_battery, metrics.status.overload,
           metrics.status.replace_battery, metrics.status.boost, metrics.status.trim);

    return 0;
}
