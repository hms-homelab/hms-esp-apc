#include "hid_debug.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "hid_debug";

static hid_dbg_report_t reports[HID_DBG_MAX_REPORTS];
static int report_count = 0;

static uint8_t desc_buf[HID_DBG_DESC_MAX];
static size_t  desc_len = 0;

static SemaphoreHandle_t lock = NULL;

static void ensure_lock(void)
{
    if (lock == NULL) {
        lock = xSemaphoreCreateMutex();
    }
}

void hid_debug_record(uint8_t report_id, const uint8_t *data, size_t len, hid_dbg_src_t src)
{
    if (data == NULL || len == 0) return;

    ensure_lock();
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;

    hid_dbg_report_t *slot = NULL;
    for (int i = 0; i < report_count; i++) {
        if (reports[i].report_id == report_id && reports[i].src == (uint8_t)src) {
            slot = &reports[i];
            break;
        }
    }
    if (slot == NULL) {
        if (report_count >= HID_DBG_MAX_REPORTS) {
            xSemaphoreGive(lock);
            return;
        }
        slot = &reports[report_count++];
        memset(slot, 0, sizeof(*slot));
        slot->report_id = report_id;
        slot->src = (uint8_t)src;
    }

    uint8_t n = (len > HID_DBG_MAX_BYTES) ? HID_DBG_MAX_BYTES : (uint8_t)len;
    slot->changed = (slot->count > 0) && (slot->len != n || memcmp(slot->data, data, n) != 0);
    memcpy(slot->data, data, n);
    slot->len = n;
    slot->count++;
    slot->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);

    xSemaphoreGive(lock);
}

void hid_debug_set_descriptor(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) return;

    ensure_lock();
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;

    desc_len = (len > HID_DBG_DESC_MAX) ? HID_DBG_DESC_MAX : len;
    memcpy(desc_buf, data, desc_len);

    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "HID report descriptor stored (%u bytes)", (unsigned)desc_len);
}

const uint8_t *hid_debug_get_descriptor(size_t *len_out)
{
    if (len_out) *len_out = desc_len;
    return desc_buf;
}

int hid_debug_get_reports(hid_dbg_report_t *out, int max_out)
{
    if (out == NULL || max_out <= 0) return 0;

    ensure_lock();
    if (lock == NULL || xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    int n = (report_count < max_out) ? report_count : max_out;
    memcpy(out, reports, n * sizeof(hid_dbg_report_t));

    xSemaphoreGive(lock);
    return n;
}
