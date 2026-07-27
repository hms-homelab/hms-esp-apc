#ifndef HID_DEBUG_H
#define HID_DEBUG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Raw HID capture.
 *
 * The parser decodes reports with hardcoded bit positions that were guessed
 * from a NUT exploration, not from the device's own report descriptor. When a
 * flag comes out wrong (e.g. LB asserted at 97% charge) there is no way to tell
 * a bad bit position from bad data, because the deployed boards have no serial
 * console — their native USB is the host link to the UPS.
 *
 * So we keep the last raw bytes of every report ID plus the HID report
 * descriptor itself, and serve them over HTTP at /hid.
 */

#define HID_DBG_MAX_REPORTS   48   /* distinct report IDs tracked */
#define HID_DBG_MAX_BYTES     32   /* bytes kept per report */
#define HID_DBG_DESC_MAX     2048  /* HID report descriptor buffer */

typedef enum {
    HID_DBG_SRC_INTERRUPT = 0,  /* device pushed it on the interrupt IN endpoint */
    HID_DBG_SRC_FEATURE   = 1,  /* we asked for it with GET_REPORT(Feature) */
} hid_dbg_src_t;

typedef struct {
    uint8_t  report_id;
    uint8_t  len;                       /* bytes actually stored */
    uint8_t  data[HID_DBG_MAX_BYTES];
    uint8_t  src;                       /* hid_dbg_src_t */
    uint32_t count;                     /* times seen since boot */
    uint32_t last_seen_ms;              /* uptime when last seen */
    bool     changed;                   /* payload differed from the previous one */
} hid_dbg_report_t;

/* Record one raw report. Safe to call from the USB task. */
void hid_debug_record(uint8_t report_id, const uint8_t *data, size_t len, hid_dbg_src_t src);

/* Store the HID report descriptor fetched at enumeration. */
void hid_debug_set_descriptor(const uint8_t *data, size_t len);

const uint8_t *hid_debug_get_descriptor(size_t *len_out);

/* Snapshot of the report table. Returns number of entries written. */
int hid_debug_get_reports(hid_dbg_report_t *out, int max_out);

#endif /* HID_DEBUG_H */
