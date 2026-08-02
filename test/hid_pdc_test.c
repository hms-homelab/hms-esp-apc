/*
 * Host-side test for the HID report descriptor walker.
 *
 * Build and run natively, no board and no ESP-IDF needed:
 *   cc -Wall -Wextra -I../main -o /tmp/hid_pdc_test hid_pdc_test.c ../main/hid_pdc.c && /tmp/hid_pdc_test
 *
 * WHY A SYNTHETIC DESCRIPTOR
 * --------------------------
 * The descriptor below is assembled by hand, so ground truth is known exactly:
 * every bit offset, size, exponent and usage is written here in the source. A
 * captured descriptor from a real device cannot do that, because the thing it
 * would be checked against is the parser under test.
 *
 * The layout deliberately mirrors what the deployed APC actually reports, as
 * documented in apc_hid_parser.c (report 0x0C charge+runtime, 0x09 battery
 * voltage at exponent -2, 0x16 PresentStatus with 11 flags then padding). So a
 * pass here means the walker reproduces the hand-decoded map that v1.14.1
 * arrived at the hard way.
 *
 * It also exercises what hand-decoding never had to handle: sign extension,
 * constant padding that must advance the bit cursor without being stored,
 * per-report cursor isolation, and Push/Pop.
 */

#include "hid_pdc.h"
#include "ups_map.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, fmt, ...)                                            \
    do {                                                                 \
        checks++;                                                        \
        if (!(cond)) {                                                   \
            failures++;                                                  \
            printf("  FAIL  " fmt "\n", ##__VA_ARGS__);                  \
        }                                                                \
    } while (0)

/* ── The descriptor ──────────────────────────────────────────────────── */

static const uint8_t kDesc[] = {
    0x05, 0x84,             /* Usage Page (Power Device)                  */
    0x09, 0x04,             /* Usage (UPS)                                */
    0xA1, 0x01,             /* Collection (Application)                   */

    0x09, 0x24,             /*   Usage (PowerSummary)                     */
    0xA1, 0x02,             /*   Collection (Logical)                     */

    /* ── Report 0x0C: RemainingCapacity (8b) then RunTimeToEmpty (16b) ── */
    0x85, 0x0C,             /*     Report ID (0x0C)                       */
    0x05, 0x85,             /*     Usage Page (Battery System)            */
    0x09, 0x66,             /*     Usage (RemainingCapacity)              */
    0x15, 0x00,             /*     Logical Minimum (0)                    */
    0x25, 0x64,             /*     Logical Maximum (100)                  */
    0x75, 0x08,             /*     Report Size (8)                        */
    0x95, 0x01,             /*     Report Count (1)                       */
    0xB1, 0x02,             /*     Feature (Data,Var,Abs)                 */
    0x09, 0x68,             /*     Usage (RunTimeToEmpty)                 */
    0x27, 0xFF, 0xFF, 0x00, 0x00, /* Logical Maximum (65535)              */
    0x75, 0x10,             /*     Report Size (16)                       */
    0x95, 0x01,             /*     Report Count (1)                       */
    0xB1, 0x02,             /*     Feature (Data,Var,Abs)                 */

    /* ── Report 0x09: battery voltage, exponent -2 ───────────────────── */
    0x85, 0x09,             /*     Report ID (0x09)                       */
    0x05, 0x84,             /*     Usage Page (Power Device)              */
    0x09, 0x30,             /*     Usage (Voltage)                        */
    0x15, 0x00,             /*     Logical Minimum (0)                    */
    0x27, 0xFF, 0xFF, 0x00, 0x00, /* Logical Maximum (65535)              */
    0x55, 0x0E,             /*     Unit Exponent (-2)                     */
    0x75, 0x10,             /*     Report Size (16)                       */
    0x95, 0x01,             /*     Report Count (1)                       */
    0xB1, 0x02,             /*     Feature (Data,Var,Abs)                 */
    0x55, 0x00,             /*     Unit Exponent (0)  -- restore          */

    /* ── Report 0x15: signed shutdown delay, -1 means inactive ───────── */
    0x85, 0x15,             /*     Report ID (0x15)                       */
    0x09, 0x57,             /*     Usage (DelayBeforeShutdown)            */
    0x15, 0xFF,             /*     Logical Minimum (-1)                   */
    0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767)                */
    0x75, 0x10,             /*     Report Size (16)                       */
    0x95, 0x01,             /*     Report Count (1)                       */
    0xB1, 0x02,             /*     Feature (Data,Var,Abs)                 */

    /* ── Report 0x16: PresentStatus, 11 flags then 21 bits of padding ── */
    0x85, 0x16,             /*     Report ID (0x16)                       */
    0x09, 0x02,             /*     Usage (PresentStatus)                  */
    0xA1, 0x02,             /*     Collection (Logical)                   */
    0x15, 0x00,             /*       Logical Minimum (0)                  */
    0x25, 0x01,             /*       Logical Maximum (1)                  */
    0x75, 0x01,             /*       Report Size (1)                      */
    0x95, 0x01,             /*       Report Count (1)                     */
    0x05, 0x85,             /*       Usage Page (Battery System)          */
    0x09, 0x44,             /*       Usage (Charging)              bit 0  */
    0xB1, 0x02,
    0x09, 0x45,             /*       Usage (Discharging)           bit 1  */
    0xB1, 0x02,
    0x09, 0xD0,             /*       Usage (ACPresent)             bit 2  */
    0xB1, 0x02,
    0x09, 0xD1,             /*       Usage (BatteryPresent)        bit 3  */
    0xB1, 0x02,
    0x09, 0x42,             /*       Usage (BelowRemainingCapacityLimit) bit 4 */
    0xB1, 0x02,
    0x05, 0x84,             /*       Usage Page (Power Device)            */
    0x09, 0x69,             /*       Usage (ShutdownImminent)      bit 5  */
    0xB1, 0x02,
    0x05, 0x85,             /*       Usage Page (Battery System)          */
    0x09, 0x43,             /*       Usage (RemainingTimeLimitExpired) bit 6 */
    0xB1, 0x02,
    0x05, 0x84,             /*       Usage Page (Power Device)            */
    0x09, 0x73,             /*       Usage (CommunicationLost)     bit 7  */
    0xB1, 0x02,
    0x05, 0x85,             /*       Usage Page (Battery System)          */
    0x09, 0x4B,             /*       Usage (NeedReplacement)       bit 8  */
    0xB1, 0x02,
    0x05, 0x84,             /*       Usage Page (Power Device)            */
    0x09, 0x65,             /*       Usage (Overload)              bit 9  */
    0xB1, 0x02,
    0x05, 0x85,             /*       Usage Page (Battery System)          */
    0x09, 0xDB,             /*       Usage (VoltageNotRegulated)   bit 10 */
    0xB1, 0x02,
    0x95, 0x15,             /*       Report Count (21)                    */
    0xB1, 0x03,             /*       Feature (Const,Var,Abs)  -- padding  */
    0xC0,                   /*     End Collection                         */

    /* ── Push/Pop around a nested block, and a Usage range ───────────── */
    0x85, 0x40,             /*     Report ID (0x40)                       */
    0x05, 0x84,             /*     Usage Page (Power Device)              */
    0x75, 0x10,             /*     Report Size (16)   <- state to preserve */
    0xA4,                   /*     Push                                   */
    0x19, 0x60,             /*     Usage Minimum (Present)                */
    0x29, 0x62,             /*     Usage Maximum (InternalFailure)        */
    0x15, 0x00,             /*     Logical Minimum (0)                    */
    0x25, 0x01,             /*     Logical Maximum (1)                    */
    0x75, 0x01,             /*     Report Size (1)    <- clobbered inside */
    0x95, 0x03,             /*     Report Count (3)                       */
    0xB1, 0x02,             /*     Feature (Data,Var,Abs)   bits 0,1,2    */
    0xB4,                   /*     Pop                                    */

    /* After the Pop both Report Size and Usage Page must be back to the
     * pre-Push values (16 and 0x84), not the 1 and 0x84 set inside. */
    0x09, 0x35,             /*     Usage (PercentLoad)                    */
    0x95, 0x01,             /*     Report Count (1)                       */
    0xB1, 0x02,             /*     Feature (Data,Var,Abs)   16 bits @ 3   */

    /* ── Report 0x01: iProduct, a STRING INDEX not text ──────────────── */
    0x85, 0x01,             /*     Report ID (0x01)                       */
    0x09, 0xFE,             /*     Usage (iProduct)                       */
    0x15, 0x00,             /*     Logical Minimum (0)                    */
    0x25, 0xFF,             /*     Logical Maximum (255)                  */
    0x75, 0x08,             /*     Report Size (8)                        */
    0x95, 0x01,             /*     Report Count (1)                       */
    0xB1, 0x02,             /*     Feature (Data,Var,Abs)                 */

    0xC0,                   /*   End Collection (PowerSummary)            */
    0xC0,                   /* End Collection (Application)               */
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

#define PAGE_PWR HID_PAGE_POWER_DEVICE
#define PAGE_BAT HID_PAGE_BATTERY

static hid_pdc_map_t map;

static const hid_pdc_field_t *feat(uint16_t apage, uint16_t aid,
                                   uint16_t lpage, uint16_t lid)
{
    return hid_pdc_find(&map, HID_PDC_FEATURE, apage, aid, lpage, lid);
}

/*
 * Stands in for the USB string descriptor fetch. Returns a name longer than
 * every char[] member in ups_metrics_t so truncation behaviour is observable.
 */
static bool fake_get_string(uint8_t index, char *out, size_t out_size, void *ctx)
{
    (void)ctx;
    if (index == 0) return false;   /* index 0 is "no string" */
    snprintf(out, out_size, "Back-UPS XS 1000M FW:945.d11 .D USB FW:0000");
    return true;
}

/* Assert a status flag sits at the expected bit within report 0x16. */
static void check_flag(const char *name, uint16_t page, uint16_t id, int expect_bit)
{
    const hid_pdc_field_t *f = feat(PAGE_PWR, 0x02, page, id);
    CHECK(f != NULL, "%s not found", name);
    if (f == NULL) return;
    CHECK(f->report_id == 0x16, "%s report_id = 0x%02X, want 0x16", name, f->report_id);
    CHECK(f->bit_size == 1, "%s bit_size = %u, want 1", name, f->bit_size);
    CHECK(f->bit_offset == expect_bit, "%s bit_offset = %u, want %d",
          name, f->bit_offset, expect_bit);
}

int main(void)
{
    printf("hid_pdc walker test (%zu byte descriptor)\n\n", sizeof(kDesc));

    /* ── Parse ───────────────────────────────────────────────────────── */
    CHECK(hid_pdc_parse(kDesc, sizeof(kDesc), &map), "parse returned false");
    CHECK(map.has_power_page, "has_power_page not set");
    CHECK(map.uses_report_ids, "uses_report_ids not set");
    CHECK(!map.truncated, "field table truncated (count=%d)", map.count);
    printf("parsed %d fields\n", map.count);

    /* ── Report 0x0C: charge + runtime ───────────────────────────────── */
    const hid_pdc_field_t *cap = feat(PAGE_PWR, 0x24, PAGE_BAT, 0x66);
    CHECK(cap != NULL, "RemainingCapacity not found");
    if (cap) {
        CHECK(cap->report_id == 0x0C, "RemainingCapacity report 0x%02X, want 0x0C", cap->report_id);
        CHECK(cap->bit_offset == 0, "RemainingCapacity bit_offset %u, want 0", cap->bit_offset);
        CHECK(cap->bit_size == 8, "RemainingCapacity bit_size %u, want 8", cap->bit_size);
        CHECK(cap->logical_max == 100, "RemainingCapacity logical_max %d, want 100", cap->logical_max);
    }

    const hid_pdc_field_t *rt = feat(PAGE_PWR, 0x24, PAGE_BAT, 0x68);
    CHECK(rt != NULL, "RunTimeToEmpty not found");
    if (rt) {
        CHECK(rt->report_id == 0x0C, "RunTimeToEmpty report 0x%02X, want 0x0C", rt->report_id);
        /* Follows the 8-bit capacity field in the same report. */
        CHECK(rt->bit_offset == 8, "RunTimeToEmpty bit_offset %u, want 8", rt->bit_offset);
        CHECK(rt->bit_size == 16, "RunTimeToEmpty bit_size %u, want 16", rt->bit_size);
        /* 65535 must not read back as -1. */
        CHECK(rt->logical_max == 65535, "RunTimeToEmpty logical_max %d, want 65535", rt->logical_max);
    }

    /* ── Report 0x09: battery voltage, exponent -2 ───────────────────── */
    const hid_pdc_field_t *volt = feat(PAGE_PWR, 0x24, PAGE_PWR, 0x30);
    CHECK(volt != NULL, "Voltage not found");
    if (volt) {
        CHECK(volt->report_id == 0x09, "Voltage report 0x%02X, want 0x09", volt->report_id);
        /* Separate report, so its cursor starts at 0 rather than continuing
         * from report 0x0C. This is the per-report cursor isolation check. */
        CHECK(volt->bit_offset == 0, "Voltage bit_offset %u, want 0", volt->bit_offset);
        CHECK(volt->unit_exponent == -2, "Voltage unit_exponent %d, want -2", volt->unit_exponent);
    }

    /* ── PresentStatus bit order ─────────────────────────────────────── */
    /* This is the exact order apc_hid_parser.c:285 documents, and getting it
     * shifted by one is what put a permanent LB on a healthy UPS in v1.14.0. */
    check_flag("Charging",                   PAGE_BAT, 0x44,  0);
    check_flag("Discharging",                PAGE_BAT, 0x45,  1);
    check_flag("ACPresent",                  PAGE_BAT, 0xD0,  2);
    check_flag("BatteryPresent",             PAGE_BAT, 0xD1,  3);
    check_flag("BelowRemainingCapacityLimit",PAGE_BAT, 0x42,  4);
    check_flag("ShutdownImminent",           PAGE_PWR, 0x69,  5);
    check_flag("RemainingTimeLimitExpired",  PAGE_BAT, 0x43,  6);
    check_flag("CommunicationLost",          PAGE_PWR, 0x73,  7);
    check_flag("NeedReplacement",            PAGE_BAT, 0x4B,  8);
    check_flag("Overload",                   PAGE_PWR, 0x65,  9);
    check_flag("VoltageNotRegulated",        PAGE_BAT, 0xDB, 10);

    /* Padding must be dropped, not stored: 11 flags, not 32. */
    int n16 = 0;
    for (int i = 0; i < map.count; i++) {
        if (map.fields[i].report_id == 0x16) n16++;
    }
    CHECK(n16 == 11, "report 0x16 has %d stored fields, want 11 (padding must be skipped)", n16);

    /* ── Push/Pop ────────────────────────────────────────────────────── */
    /* PercentLoad comes after the Pop, so it must inherit the Report Size of
     * 16 that was in force before the Push, not the 1 set inside it. */
    const hid_pdc_field_t *load = feat(PAGE_PWR, 0x24, PAGE_PWR, 0x35);
    CHECK(load != NULL, "PercentLoad not found");
    if (load) {
        CHECK(load->bit_size == 16, "PercentLoad bit_size %u, want 16 (Pop did not restore)",
              load->bit_size);
        CHECK(load->bit_offset == 3, "PercentLoad bit_offset %u, want 3 (after 3 range bits)",
              load->bit_offset);
    }

    /* ── Usage Minimum/Maximum range ─────────────────────────────────── */
    const hid_pdc_field_t *good = feat(PAGE_PWR, 0x24, PAGE_PWR, 0x61);  /* Good */
    CHECK(good != NULL, "Usage range did not produce Good (0x84:0x61)");
    if (good) {
        CHECK(good->bit_offset == 1, "Good bit_offset %u, want 1", good->bit_offset);
    }

    /* ── Value extraction ────────────────────────────────────────────── */
    int32_t raw;

    /* Report 0x16 payload 0x0D 0x00 -> bits 0,2,3 set.
     * Charging on, Discharging off, ACPresent on, BatteryPresent on. */
    const uint8_t status_payload[] = { 0x0D, 0x00 };
    const hid_pdc_field_t *chg = feat(PAGE_PWR, 0x02, PAGE_BAT, 0x44);
    const hid_pdc_field_t *dis = feat(PAGE_PWR, 0x02, PAGE_BAT, 0x45);
    const hid_pdc_field_t *acp = feat(PAGE_PWR, 0x02, PAGE_BAT, 0xD0);
    const hid_pdc_field_t *lowb= feat(PAGE_PWR, 0x02, PAGE_BAT, 0x42);
    if (chg && hid_pdc_extract(chg, status_payload, sizeof(status_payload), &raw))
        CHECK(raw == 1, "Charging extracted %d, want 1", raw);
    if (dis && hid_pdc_extract(dis, status_payload, sizeof(status_payload), &raw))
        CHECK(raw == 0, "Discharging extracted %d, want 0", raw);
    if (acp && hid_pdc_extract(acp, status_payload, sizeof(status_payload), &raw))
        CHECK(raw == 1, "ACPresent extracted %d, want 1", raw);
    /* The v1.14.0 bug in one assertion: BatteryPresent must not read as low battery. */
    if (lowb && hid_pdc_extract(lowb, status_payload, sizeof(status_payload), &raw))
        CHECK(raw == 0, "BelowRemainingCapacityLimit extracted %d, want 0 "
                        "(reading BatteryPresent here is the v1.14.0 LB bug)", raw);

    /* Battery voltage: 0x04B0 = 1200, exponent -2 -> 12.00 V.
     * apc_hid_parser.c:193 documents this exact device reading. */
    const uint8_t volt_payload[] = { 0xB0, 0x04 };
    if (volt && hid_pdc_extract(volt, volt_payload, sizeof(volt_payload), &raw)) {
        CHECK(raw == 1200, "Voltage raw %d, want 1200", raw);
        float v = hid_pdc_scale(volt, raw);
        CHECK(v > 11.99f && v < 12.01f, "Voltage scaled %.3f, want 12.00", (double)v);
    }

    /* Charge + runtime out of one payload: 100%, 300 s. */
    const uint8_t cr_payload[] = { 0x64, 0x2C, 0x01 };
    if (cap && hid_pdc_extract(cap, cr_payload, sizeof(cr_payload), &raw))
        CHECK(raw == 100, "battery charge %d, want 100", raw);
    if (rt && hid_pdc_extract(rt, cr_payload, sizeof(cr_payload), &raw))
        CHECK(raw == 300, "runtime %d, want 300", raw);

    /* Sign extension: 0xFFFF with logical_min -1 is -1, not 65535. */
    const hid_pdc_field_t *dly = feat(PAGE_PWR, 0x24, PAGE_PWR, 0x57);
    CHECK(dly != NULL, "DelayBeforeShutdown not found");
    if (dly) {
        CHECK(dly->logical_min == -1, "DelayBeforeShutdown logical_min %d, want -1",
              dly->logical_min);
        const uint8_t neg[] = { 0xFF, 0xFF };
        if (hid_pdc_extract(dly, neg, sizeof(neg), &raw))
            CHECK(raw == -1, "DelayBeforeShutdown extracted %d, want -1", raw);
    }

    /* Short report must be refused, not read past. */
    const uint8_t truncated_payload[] = { 0x64 };
    if (rt) {
        CHECK(!hid_pdc_extract(rt, truncated_payload, sizeof(truncated_payload), &raw),
              "extract accepted a report shorter than the field");
    }

    /* ── Path formatting ─────────────────────────────────────────────── */
    if (chg) {
        char path[128];
        hid_pdc_format_path(chg, path, sizeof(path));
        CHECK(strcmp(path, "UPS.PowerSummary.PresentStatus.Charging") == 0,
              "path = \"%s\", want \"UPS.PowerSummary.PresentStatus.Charging\"", path);
    }

    /* ── ups_map: usage paths land in the right metrics fields ───────── */
    {
        ups_metrics_t m;
        memset(&m, 0, sizeof(m));

        CHECK(ups_map_is_usable(&map), "ups_map_is_usable false for a PDC descriptor");

        int n = ups_map_apply(&map, 0x0C, HID_PDC_FEATURE, cr_payload, sizeof(cr_payload),
                              &m, NULL, NULL);
        CHECK(n == 2, "report 0x0C applied %d fields, want 2", n);
        CHECK(m.battery_charge > 99.9f && m.battery_charge < 100.1f,
              "battery_charge %.2f, want 100", (double)m.battery_charge);
        CHECK(m.battery_runtime > 299.9f && m.battery_runtime < 300.1f,
              "battery_runtime %.2f, want 300", (double)m.battery_runtime);

        ups_map_apply(&map, 0x09, HID_PDC_FEATURE, volt_payload, sizeof(volt_payload),
                      &m, NULL, NULL);
        CHECK(m.battery_voltage > 11.99f && m.battery_voltage < 12.01f,
              "battery_voltage %.3f, want 12.00 (exponent -2 applied)", (double)m.battery_voltage);

        ups_map_apply(&map, 0x16, HID_PDC_FEATURE, status_payload, sizeof(status_payload),
                      &m, NULL, NULL);
        CHECK(m.status.charging,     "status.charging false, want true");
        CHECK(!m.status.discharging, "status.discharging true, want false");
        CHECK(m.status.online,       "status.online false, want true (ACPresent)");
        /* The v1.14.0 regression, restated at the mapping layer. */
        CHECK(!m.status.low_battery,
              "status.low_battery true at 100%% charge -- BatteryPresent misread as LB again");
        CHECK(m.valid, "metrics not marked valid after applying reports");

        /* Derived poll list replaces the hardcoded poll_reports[]. */
        uint8_t ids[16];
        int nids = ups_map_feature_report_ids(&map, ids, 16);
        CHECK(nids == 6, "derived %d feature report ids, want 6 (0x0C,0x09,0x15,0x16,0x40,0x01)", nids);
        bool saw_0c = false, saw_16 = false;
        for (int i = 0; i < nids; i++) {
            if (ids[i] == 0x0C) saw_0c = true;
            if (ids[i] == 0x16) saw_16 = true;
        }
        CHECK(saw_0c && saw_16, "derived poll list missing 0x0C or 0x16");
    }

    /* ── ups_map: string indices resolve, and are not clamped to the
     *    shortest char[] member. ups_model is char[40] while battery_type
     *    is char[16]; clamping everything to 16 was a real bug in the first
     *    cut of ups_map.c and would have silently truncated every model
     *    name. ──────────────────────────────────────────────────────── */
    {
        ups_metrics_t m;
        memset(&m, 0, sizeof(m));

        const uint8_t idx_payload[] = { 0x02 };   /* string index 2 */
        int n = ups_map_apply(&map, 0x01, HID_PDC_FEATURE,
                              idx_payload, sizeof(idx_payload),
                              &m, fake_get_string, NULL);
        CHECK(n == 1, "report 0x01 applied %d fields, want 1", n);

        /* fake_get_string returns a 45-char name. ups_model holds 40 bytes, so
         * 39 chars survive. The bug would have left 15. */
        CHECK(strlen(m.ups_model) == 39,
              "ups_model kept %zu chars, want 39 (16 means the clamp bug is back)",
              strlen(m.ups_model));
        CHECK(strncmp(m.ups_model, "Back-UPS", 8) == 0,
              "ups_model = \"%s\", want it to start with Back-UPS", m.ups_model);

        /* Index 0 means "no string" and must not fetch anything. */
        memset(&m, 0, sizeof(m));
        const uint8_t zero_idx[] = { 0x00 };
        n = ups_map_apply(&map, 0x01, HID_PDC_FEATURE, zero_idx, sizeof(zero_idx),
                          &m, fake_get_string, NULL);
        CHECK(n == 0, "string index 0 applied %d fields, want 0", n);
        CHECK(m.ups_model[0] == '\0', "string index 0 wrote \"%s\"", m.ups_model);

        /* A NULL fetcher must be tolerated, not crash. */
        n = ups_map_apply(&map, 0x01, HID_PDC_FEATURE, idx_payload, sizeof(idx_payload),
                          &m, NULL, NULL);
        CHECK(n == 0, "NULL get_string applied %d fields, want 0", n);
    }

    /* ── HID unit dimensional scale ──────────────────────────────────────
     * REGRESSION. HID's SI Linear system is centimetre-gram-second, so a
     * quantity's HID base unit differs from its SI one by a power of ten.
     * Ignoring it made a real APC's 13.71 V battery decode as 137100000 V,
     * because that device declares Unit Exponent 5 and expects the reader to
     * add the unit scale of -7. Every descriptor written by hand for a test
     * hides this; only a real one exposes it. ────────────────────────────── */
    {
        /* Volt: kg*m^2*s^-3*A^-1. Nibbles (LSB first): system 1 (SI Linear),
         * length 2, mass 1, time -3 (0xD), temp 0, current -1 (0xF).
         * This exact value is what the deployed Back-UPS XS 1000M declares. */
        CHECK(hid_pdc_unit_scale_exp(0x00F0D121) == -7,
              "volt unit scale = %d, want -7", hid_pdc_unit_scale_exp(0x00F0D121));

        /* Seconds: system 1, time exponent 1, no length or mass. */
        CHECK(hid_pdc_unit_scale_exp(0x00001001) == 0,
              "second unit scale = %d, want 0", hid_pdc_unit_scale_exp(0x00001001));

        /* No unit declared at all (percentages, enums, counts). */
        CHECK(hid_pdc_unit_scale_exp(0x00000000) == 0,
              "unitless scale = %d, want 0", hid_pdc_unit_scale_exp(0x00000000));

        /* English systems are left alone rather than guessed at. */
        CHECK(hid_pdc_unit_scale_exp(0x00F0D123) == 0,
              "english unit scale = %d, want 0 (unhandled, not guessed)",
              hid_pdc_unit_scale_exp(0x00F0D123));

        /* End to end: the real APC battery voltage. Raw 1371, Unit Exponent 5,
         * volt unit -7, so 1371 * 10^-2 = 13.71 V. */
        hid_pdc_field_t vf;
        memset(&vf, 0, sizeof(vf));
        vf.unit          = 0x00F0D121;
        vf.unit_exponent = 5;
        float volts = hid_pdc_scale(&vf, 1371);
        CHECK(volts > 13.70f && volts < 13.72f,
              "APC battery voltage scaled to %.2f, want 13.71", (double)volts);
    }

    /* ── Robustness: must not crash or hang on malformed input ───────── */
    hid_pdc_map_t junk;
    const uint8_t truncated_item[] = { 0x05 };          /* Usage Page, no data */
    CHECK(!hid_pdc_parse(truncated_item, sizeof(truncated_item), &junk),
          "truncated item should fail cleanly");
    const uint8_t bad_long[] = { 0xFE };                 /* long item, no length */
    CHECK(!hid_pdc_parse(bad_long, sizeof(bad_long), &junk),
          "malformed long item should fail cleanly");
    CHECK(hid_pdc_parse(kDesc, 0, &junk), "zero-length descriptor should parse to nothing");

    /*
     * Truncation sweep. A descriptor arrives over USB from a device we do not
     * control, and a short or corrupt read must not walk off the buffer. These
     * boards have no serial console, so a crash here is a trip up a ladder.
     * Run under -fsanitize=address,undefined for this to mean anything.
     */
    for (size_t n = 0; n <= sizeof(kDesc); n++) {
        hid_pdc_parse(kDesc, n, &junk);   /* must not crash; result unchecked */
    }

    /* Every single-byte corruption, same requirement. */
    static uint8_t mutated[sizeof(kDesc)];
    for (size_t pos = 0; pos < sizeof(kDesc); pos++) {
        for (unsigned v = 0; v < 256; v += 17) {
            memcpy(mutated, kDesc, sizeof(kDesc));
            mutated[pos] = (uint8_t)v;
            hid_pdc_parse(mutated, sizeof(mutated), &junk);
        }
    }
    printf("robustness: %zu truncations + %zu mutations survived\n",
           sizeof(kDesc) + 1, sizeof(kDesc) * 16);

    /* ── Result ──────────────────────────────────────────────────────── */
    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0) printf("PASS\n");
    else               printf("FAIL\n");
    return failures == 0 ? 0 : 1;
}
