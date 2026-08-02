#include "hid_pdc.h"
#include <string.h>
#include <stdio.h>

/*
 * HID report descriptor walker. See hid_pdc.h for why this exists.
 *
 * Reference: USB Device Class Definition for HID 1.11 section 6.2.2, and
 * "Universal Serial Bus Usage Tables for HID Power Devices" release 1.0.
 *
 * No ESP-IDF dependencies on purpose, so test/hid_pdc_test.c can compile this
 * file natively and check it against a real captured descriptor before any of
 * it reaches a board.
 */

/* ══════════════════ Item decoding ══════════════════ */

/* bType */
#define ITEM_MAIN    0
#define ITEM_GLOBAL  1
#define ITEM_LOCAL   2

/* Main item bTags */
#define MAIN_INPUT           0x8
#define MAIN_OUTPUT          0x9
#define MAIN_FEATURE         0xB
#define MAIN_COLLECTION      0xA
#define MAIN_END_COLLECTION  0xC

/* Global item bTags */
#define GLOBAL_USAGE_PAGE    0x0
#define GLOBAL_LOGICAL_MIN   0x1
#define GLOBAL_LOGICAL_MAX   0x2
#define GLOBAL_PHYSICAL_MIN  0x3
#define GLOBAL_PHYSICAL_MAX  0x4
#define GLOBAL_UNIT_EXPONENT 0x5
#define GLOBAL_UNIT          0x6
#define GLOBAL_REPORT_SIZE   0x7
#define GLOBAL_REPORT_ID     0x8
#define GLOBAL_REPORT_COUNT  0x9
#define GLOBAL_PUSH          0xA
#define GLOBAL_POP           0xB

/* Local item bTags */
#define LOCAL_USAGE          0x0
#define LOCAL_USAGE_MIN      0x1
#define LOCAL_USAGE_MAX      0x2

static uint32_t item_udata(const uint8_t *p, int size)
{
    switch (size) {
        case 1:  return p[0];
        case 2:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        case 4:  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        default: return 0;
    }
}

static int32_t item_sdata(const uint8_t *p, int size)
{
    uint32_t v = item_udata(p, size);
    switch (size) {
        case 1:  return (int32_t)(int8_t)v;
        case 2:  return (int32_t)(int16_t)v;
        case 4:  return (int32_t)v;
        default: return 0;
    }
}

/* ══════════════════ Parser state ══════════════════ */

typedef struct {
    uint16_t usage_page;
    int32_t  logical_min;
    int32_t  logical_max_s;   /* sign-extended reading */
    uint32_t logical_max_u;   /* raw reading */
    int8_t   unit_exponent;
    uint32_t unit;
    uint8_t  report_size;
    uint8_t  report_count;
    uint8_t  report_id;
} global_state_t;

typedef struct {
    hid_usage_t usages[HID_PDC_MAX_USAGES];
    int         usage_count;
    bool        has_range;
    hid_usage_t usage_min;
    hid_usage_t usage_max;
} local_state_t;

static void locals_reset(local_state_t *l)
{
    l->usage_count = 0;
    l->has_range   = false;
}

/*
 * Which usage applies to field n of a Variable main item.
 *
 * If explicit Usage items were given they are consumed in order and the LAST
 * one repeats for any remaining fields; that repetition is standard and is how
 * descriptors declare "8 more of the same thing". A Usage Minimum/Maximum range
 * instead numbers them sequentially.
 */
static hid_usage_t usage_for_index(const local_state_t *l, const global_state_t *g, int n)
{
    hid_usage_t u;

    if (l->usage_count > 0) {
        int idx = (n < l->usage_count) ? n : (l->usage_count - 1);
        return l->usages[idx];
    }
    if (l->has_range) {
        uint32_t id = (uint32_t)l->usage_min.id + (uint32_t)n;
        if (id > l->usage_max.id) id = l->usage_max.id;
        u.page = l->usage_min.page;
        u.id   = (uint16_t)id;
        return u;
    }
    u.page = g->usage_page;
    u.id   = 0;
    return u;
}

/* Usage items may carry the page in their top 16 bits (bSize 4) or inherit the
 * current global Usage Page. Getting this wrong silently reassigns every field
 * to the wrong page. */
static hid_usage_t make_usage(uint32_t raw, int size, uint16_t current_page)
{
    hid_usage_t u;
    if (size == 4) {
        u.page = (uint16_t)(raw >> 16);
        u.id   = (uint16_t)(raw & 0xFFFF);
    } else {
        u.page = current_page;
        u.id   = (uint16_t)(raw & 0xFFFF);
    }
    return u;
}

bool hid_pdc_parse(const uint8_t *desc, size_t desc_len, hid_pdc_map_t *map)
{
    if (desc == NULL || map == NULL) return false;

    memset(map, 0, sizeof(*map));

    /*
     * Bit cursor per (report type, report ID). Fields accumulate independently
     * for each report, which is the classic place these parsers go wrong: share
     * one cursor and every report after the first is offset by the length of
     * the ones before it.
     *
     * Static rather than on the stack (1.5 KB) because the USB task's stack is
     * not generous. hid_pdc_parse is therefore NOT reentrant; it is called once
     * per device at enumeration.
     */
    static uint16_t cursor[3][256];
    memset(cursor, 0, sizeof(cursor));

    global_state_t g;
    memset(&g, 0, sizeof(g));

    global_state_t gstack[4];
    int gdepth = 0;

    local_state_t l;
    locals_reset(&l);

    hid_usage_t coll[HID_PDC_MAX_COLL];
    int coll_depth = 0;

    size_t i = 0;
    while (i < desc_len) {
        uint8_t prefix = desc[i++];

        /* Long items carry no information we use, but they must be stepped
         * over exactly or the rest of the descriptor decodes as garbage. */
        if (prefix == 0xFE) {
            if (i + 1 >= desc_len) return false;
            uint8_t dsize = desc[i];
            i += (size_t)2 + dsize;
            continue;
        }

        int bsize = prefix & 0x03;
        if (bsize == 3) bsize = 4;
        int btype = (prefix >> 2) & 0x03;
        int btag  = (prefix >> 4) & 0x0F;

        if (i + (size_t)bsize > desc_len) return false;
        const uint8_t *data = &desc[i];
        i += (size_t)bsize;

        uint32_t udata = item_udata(data, bsize);
        int32_t  sdata = item_sdata(data, bsize);

        switch (btype) {

        case ITEM_GLOBAL:
            switch (btag) {
            case GLOBAL_USAGE_PAGE:
                g.usage_page = (uint16_t)(udata & 0xFFFF);
                if (g.usage_page == HID_PAGE_POWER_DEVICE) map->has_power_page = true;
                break;
            case GLOBAL_LOGICAL_MIN:   g.logical_min   = sdata; break;
            case GLOBAL_LOGICAL_MAX:   g.logical_max_s = sdata;
                                       g.logical_max_u = udata; break;
            case GLOBAL_PHYSICAL_MIN:  break;  /* unused: we scale by exponent */
            case GLOBAL_PHYSICAL_MAX:  break;
            case GLOBAL_UNIT_EXPONENT: {
                /* 4-bit two's complement nibble: 0x0-0x7 are 0..7, 0x8-0xF are -8..-1 */
                uint8_t nib = (uint8_t)(udata & 0x0F);
                g.unit_exponent = (nib > 7) ? (int8_t)(nib - 16) : (int8_t)nib;
                break;
            }
            case GLOBAL_UNIT:          g.unit         = udata; break;
            case GLOBAL_REPORT_SIZE:   g.report_size  = (uint8_t)udata; break;
            case GLOBAL_REPORT_ID:     g.report_id    = (uint8_t)udata;
                                       map->uses_report_ids = true; break;
            case GLOBAL_REPORT_COUNT:  g.report_count = (uint8_t)udata; break;
            case GLOBAL_PUSH:
                if (gdepth < (int)(sizeof(gstack) / sizeof(gstack[0]))) gstack[gdepth++] = g;
                break;
            case GLOBAL_POP:
                if (gdepth > 0) g = gstack[--gdepth];
                break;
            default: break;
            }
            break;

        case ITEM_LOCAL:
            switch (btag) {
            case LOCAL_USAGE:
                if (l.usage_count < HID_PDC_MAX_USAGES) {
                    l.usages[l.usage_count++] = make_usage(udata, bsize, g.usage_page);
                }
                break;
            case LOCAL_USAGE_MIN:
                l.usage_min = make_usage(udata, bsize, g.usage_page);
                l.has_range = true;
                break;
            case LOCAL_USAGE_MAX:
                l.usage_max = make_usage(udata, bsize, g.usage_page);
                l.has_range = true;
                break;
            default: break;  /* designators, string indices, delimiters */
            }
            break;

        case ITEM_MAIN:
            switch (btag) {

            case MAIN_COLLECTION: {
                hid_usage_t cu;
                if (l.usage_count > 0) {
                    cu = l.usages[0];
                } else {
                    cu.page = g.usage_page;
                    cu.id   = 0;
                }
                if (coll_depth < HID_PDC_MAX_COLL) coll[coll_depth] = cu;
                coll_depth++;               /* count past the cap so End matches */
                locals_reset(&l);
                break;
            }

            case MAIN_END_COLLECTION:
                if (coll_depth > 0) coll_depth--;
                locals_reset(&l);
                break;

            case MAIN_INPUT:
            case MAIN_OUTPUT:
            case MAIN_FEATURE: {
                uint8_t rtype = (btag == MAIN_INPUT)  ? HID_PDC_INPUT  :
                                (btag == MAIN_OUTPUT) ? HID_PDC_OUTPUT :
                                                        HID_PDC_FEATURE;
                uint16_t *cur = &cursor[rtype][g.report_id];

                bool is_constant = (udata & 0x01) != 0;
                bool is_variable = (udata & 0x02) != 0;
                bool is_relative = (udata & 0x04) != 0;

                for (int n = 0; n < (int)g.report_count; n++) {
                    uint16_t off = *cur;
                    *cur = (uint16_t)(*cur + g.report_size);

                    /* Padding carries no data. Skip storing it, but the cursor
                     * above must still advance or every later field in this
                     * report lands at the wrong offset. */
                    if (is_constant) continue;

                    if (map->count >= HID_PDC_MAX_FIELDS) {
                        map->truncated = true;
                        continue;
                    }

                    hid_pdc_field_t *f = &map->fields[map->count++];
                    memset(f, 0, sizeof(*f));

                    f->report_id   = g.report_id;
                    f->report_type = rtype;
                    f->usage       = is_variable ? usage_for_index(&l, &g, n)
                                                 : usage_for_index(&l, &g, 0);
                    f->bit_offset  = off;
                    f->bit_size    = g.report_size;
                    f->logical_min = g.logical_min;
                    /* Logical Maximum is nominally signed, but encoding 255 as
                     * 0xFF is ubiquitous and would read as -1. Follow the rule
                     * Linux hid-core uses: signed only when the minimum is. */
                    f->logical_max = (g.logical_min < 0) ? g.logical_max_s
                                                         : (int32_t)g.logical_max_u;
                    f->unit_exponent = g.unit_exponent;
                    f->unit          = g.unit;
                    f->flags = (uint8_t)((is_constant ? HID_PDC_F_CONSTANT : 0) |
                                         (is_variable ? HID_PDC_F_VARIABLE : 0) |
                                         (is_relative ? HID_PDC_F_RELATIVE : 0));

                    int keep = (coll_depth < HID_PDC_MAX_PATH) ? coll_depth : HID_PDC_MAX_PATH;
                    for (int c = 0; c < keep; c++) f->path[c] = coll[c];
                    f->path_len = (uint8_t)coll_depth;
                }
                locals_reset(&l);
                break;
            }

            default:
                locals_reset(&l);
                break;
            }
            break;

        default:  /* Reserved bType */
            break;
        }
    }

    return true;
}

/* ══════════════════ Value extraction ══════════════════ */

bool hid_pdc_extract(const hid_pdc_field_t *f, const uint8_t *payload,
                     size_t payload_len, int32_t *out_raw)
{
    if (f == NULL || payload == NULL || out_raw == NULL) return false;
    if (f->bit_size == 0 || f->bit_size > 32) return false;

    /* A short report is normal, and reading past it is exactly how you get a
     * garbage value that still looks like a plausible reading. */
    size_t last_bit = (size_t)f->bit_offset + f->bit_size;
    if (last_bit > payload_len * 8u) return false;

    uint32_t value = 0;
    for (int b = 0; b < f->bit_size; b++) {
        size_t bit = (size_t)f->bit_offset + b;
        uint8_t byte = payload[bit >> 3];
        if (byte & (1u << (bit & 7u))) {
            value |= (1u << b);
        }
    }

    if (f->logical_min < 0 && f->bit_size < 32) {
        uint32_t sign_bit = 1u << (f->bit_size - 1);
        if (value & sign_bit) {
            value |= ~((1u << f->bit_size) - 1u);   /* sign extend */
        }
    }

    *out_raw = (int32_t)value;
    return true;
}

/* One nibble of the Unit field, as a signed 4-bit exponent. */
static int unit_nibble(uint32_t unit, int index)
{
    int v = (int)((unit >> (4 * index)) & 0xF);
    return (v > 7) ? v - 16 : v;
}

int hid_pdc_unit_scale_exp(uint32_t unit)
{
    /*
     * HID's SI Linear system is centimetre-gram-second, NOT metre-kilogram-
     * second (HID 1.11 section 6.2.2.7). So a quantity's HID base unit differs
     * from its SI one by a power of ten that depends on its dimensions:
     *
     *   length  cm -> m    10^-2 per length exponent
     *   mass    g  -> kg   10^-3 per mass exponent
     *
     * Volt = kg*m^2*s^-3*A^-1, so length exp 2 and mass exp 1 give
     * 10^-4 * 10^-3 = 10^-7. Watt works out the same.
     *
     * Ignoring this is why the first cut read a 13.71 V battery as 137100000 V:
     * the APC declares Unit Exponent 5 on that field and relies on the reader
     * applying the unit scale of -7 as well. Only a real device's descriptor
     * exposes that; a hand-written test descriptor declaring "exponent -2"
     * hides it completely.
     */
    int system = (int)(unit & 0xF);

    /* 0 = none, 1 = SI Linear, 2 = SI Rotation, 3/4 = English. Only the SI
     * systems get corrected; English units do not appear on UPS descriptors
     * and guessing at them would do more harm than leaving them alone. */
    if (system != 1 && system != 2) return 0;

    int length = unit_nibble(unit, 1);
    int mass   = unit_nibble(unit, 2);

    int e = -3 * mass;
    if (system == 1) e += -2 * length;  /* rotation's "length" is radians */
    return e;
}

float hid_pdc_scale(const hid_pdc_field_t *f, int32_t raw)
{
    float v = (float)raw;
    if (f == NULL) return v;

    int e = f->unit_exponent + hid_pdc_unit_scale_exp(f->unit);

    while (e > 0) { v *= 10.0f; e--; }
    while (e < 0) { v /= 10.0f; e++; }
    return v;
}

const hid_pdc_field_t *hid_pdc_find(const hid_pdc_map_t *map,
                                    uint8_t report_type,
                                    uint16_t ancestor_page, uint16_t ancestor_id,
                                    uint16_t leaf_page, uint16_t leaf_id)
{
    if (map == NULL) return NULL;

    for (int i = 0; i < map->count; i++) {
        const hid_pdc_field_t *f = &map->fields[i];

        if (f->report_type != report_type) continue;
        if (f->usage.page != leaf_page || f->usage.id != leaf_id) continue;

        if (ancestor_page == 0) return f;

        int keep = (f->path_len < HID_PDC_MAX_PATH) ? f->path_len : HID_PDC_MAX_PATH;
        for (int c = 0; c < keep; c++) {
            if (f->path[c].page == ancestor_page && f->path[c].id == ancestor_id) {
                return f;
            }
        }
    }
    return NULL;
}

/* ══════════════════ Usage names (for /hid output) ══════════════════ */

/*
 * Transcribed from NUT drivers/libhid.c hid_usage_lkp[], which is itself the
 * table from the USB HID Power Devices usage spec. Kept as (page<<16|id) so the
 * literals can be diffed against NUT directly.
 */
typedef struct { uint32_t code; const char *name; } usage_name_t;

static const usage_name_t kUsageNames[] = {
    /* Power Device page 0x84 */
    { 0x00840001, "iName" },
    { 0x00840002, "PresentStatus" },
    { 0x00840003, "ChangedStatus" },
    { 0x00840004, "UPS" },
    { 0x00840005, "PowerSupply" },
    { 0x00840010, "BatterySystem" },
    { 0x00840012, "Battery" },
    { 0x00840014, "Charger" },
    { 0x00840016, "PowerConverter" },
    { 0x00840018, "OutletSystem" },
    { 0x0084001a, "Input" },
    { 0x0084001c, "Output" },
    { 0x0084001e, "Flow" },
    { 0x00840020, "Outlet" },
    { 0x00840022, "Gang" },
    { 0x00840024, "PowerSummary" },
    { 0x00840030, "Voltage" },
    { 0x00840031, "Current" },
    { 0x00840032, "Frequency" },
    { 0x00840033, "ApparentPower" },
    { 0x00840034, "ActivePower" },
    { 0x00840035, "PercentLoad" },
    { 0x00840036, "Temperature" },
    { 0x00840037, "Humidity" },
    { 0x00840038, "BadCount" },
    { 0x00840040, "ConfigVoltage" },
    { 0x00840041, "ConfigCurrent" },
    { 0x00840042, "ConfigFrequency" },
    { 0x00840043, "ConfigApparentPower" },
    { 0x00840044, "ConfigActivePower" },
    { 0x00840045, "ConfigPercentLoad" },
    { 0x00840046, "ConfigTemperature" },
    { 0x00840050, "SwitchOnControl" },
    { 0x00840051, "SwitchOffControl" },
    { 0x00840052, "ToggleControl" },
    { 0x00840053, "LowVoltageTransfer" },
    { 0x00840054, "HighVoltageTransfer" },
    { 0x00840055, "DelayBeforeReboot" },
    { 0x00840056, "DelayBeforeStartup" },
    { 0x00840057, "DelayBeforeShutdown" },
    { 0x00840058, "Test" },
    { 0x00840059, "ModuleReset" },
    { 0x0084005a, "AudibleAlarmControl" },
    { 0x00840060, "Present" },
    { 0x00840061, "Good" },
    { 0x00840062, "InternalFailure" },
    { 0x00840063, "VoltageOutOfRange" },
    { 0x00840064, "FrequencyOutOfRange" },
    { 0x00840065, "Overload" },
    { 0x00840066, "OverCharged" },
    { 0x00840067, "OverTemperature" },
    { 0x00840068, "ShutdownRequested" },
    { 0x00840069, "ShutdownImminent" },
    { 0x0084006b, "SwitchOn/Off" },
    { 0x0084006c, "Switchable" },
    { 0x0084006d, "Used" },
    { 0x0084006e, "Boost" },
    { 0x0084006f, "Buck" },
    { 0x00840070, "Initialized" },
    { 0x00840071, "Tested" },
    { 0x00840072, "AwaitingPower" },
    { 0x00840073, "CommunicationLost" },
    { 0x008400fd, "iManufacturer" },
    { 0x008400fe, "iProduct" },
    { 0x008400ff, "iSerialNumber" },

    /* Battery System page 0x85 */
    { 0x00850029, "RemainingCapacityLimit" },
    { 0x0085002a, "RemainingTimeLimit" },
    { 0x0085002c, "CapacityMode" },
    { 0x00850040, "TerminateCharge" },
    { 0x00850041, "TerminateDischarge" },
    { 0x00850042, "BelowRemainingCapacityLimit" },
    { 0x00850043, "RemainingTimeLimitExpired" },
    { 0x00850044, "Charging" },
    { 0x00850045, "Discharging" },
    { 0x00850046, "FullyCharged" },
    { 0x00850047, "FullyDischarged" },
    { 0x0085004b, "NeedReplacement" },
    { 0x00850064, "RelativeStateOfCharge" },
    { 0x00850065, "AbsoluteStateOfCharge" },
    { 0x00850066, "RemainingCapacity" },
    { 0x00850067, "FullChargeCapacity" },
    { 0x00850068, "RunTimeToEmpty" },
    { 0x00850069, "AverageTimeToEmpty" },
    { 0x0085006a, "AverageTimeToFull" },
    { 0x0085006b, "CycleCount" },
    { 0x00850083, "DesignCapacity" },
    { 0x00850085, "ManufacturerDate" },
    { 0x00850086, "SerialNumber" },
    { 0x00850087, "iManufacturerName" },
    { 0x00850088, "iDevicename" },
    { 0x00850089, "iDeviceChemistry" },
    { 0x0085008b, "Rechargeable" },
    { 0x0085008c, "WarningCapacityLimit" },
    { 0x0085008d, "CapacityGranularity1" },
    { 0x0085008e, "CapacityGranularity2" },
    { 0x0085008f, "iOEMInformation" },
    { 0x008500d0, "ACPresent" },
    { 0x008500d1, "BatteryPresent" },
    { 0x008500d2, "PowerFail" },
    { 0x008500d3, "AlarmInhibited" },
    { 0x008500d8, "VoltageOutOfRange" },
    { 0x008500d9, "CurrentOutOfRange" },
    { 0x008500da, "CurrentNotRegulated" },
    { 0x008500db, "VoltageNotRegulated" },
    { 0x008500dc, "MasterMode" },

    { 0, NULL }
};

const char *hid_pdc_usage_name(uint16_t page, uint16_t id)
{
    uint32_t code = ((uint32_t)page << 16) | id;
    for (int i = 0; kUsageNames[i].name != NULL; i++) {
        if (kUsageNames[i].code == code) return kUsageNames[i].name;
    }
    return NULL;
}

void hid_pdc_format_path(const hid_pdc_field_t *f, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    out[0] = '\0';
    if (f == NULL) return;

    size_t pos = 0;
    int keep = (f->path_len < HID_PDC_MAX_PATH) ? f->path_len : HID_PDC_MAX_PATH;

    for (int c = 0; c < keep && pos < out_size; c++) {
        const char *n = hid_pdc_usage_name(f->path[c].page, f->path[c].id);
        int w;
        if (n != NULL) {
            w = snprintf(out + pos, out_size - pos, "%s%s", (pos > 0) ? "." : "", n);
        } else {
            w = snprintf(out + pos, out_size - pos, "%s%04X:%04X",
                         (pos > 0) ? "." : "", f->path[c].page, f->path[c].id);
        }
        if (w < 0) break;
        pos += (size_t)w;
        if (pos >= out_size) return;
    }

    const char *ln = hid_pdc_usage_name(f->usage.page, f->usage.id);
    if (ln != NULL) {
        snprintf(out + pos, out_size - pos, "%s%s", (pos > 0) ? "." : "", ln);
    } else {
        snprintf(out + pos, out_size - pos, "%s%04X:%04X",
                 (pos > 0) ? "." : "", f->usage.page, f->usage.id);
    }
}
