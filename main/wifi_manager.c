#include "wifi_manager.h"
#include "dns_hijack.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_ap_netif  = NULL;
static char  stored_ssid[64]    = {0};
static char  s_portal_ssid[32]  = {0};
static int   s_retry_num        = 0;
static bool  s_portal_active    = false;
static bool  s_initialized      = false;

/* Defined below, next to the scan cache it fills. Called by wifi_start_portal()
 * while still in STA-only mode, before the AP is brought up. */
static void do_scan_and_cache(void);

static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        /* Once the portal is up we stop hammering the STA interface. */
        if (s_portal_active) return;

        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d...", s_retry_num, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "❌ Giving up after %d attempts", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "✅ Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "📱 Portal client joined: " MACSTR, MAC2STR(e->mac));

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "📱 Portal client left: " MACSTR, MAC2STR(e->mac));
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Both netifs exist up front so the portal can be raised at any time
     * without tearing the driver down. */
    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    ESP_LOGI(TAG, "⚙️  Using DHCP for automatic IP assignment");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_connect_sta(const char *ssid, const char *password)
{
    ESP_ERROR_CHECK(wifi_manager_init());

    strlcpy(stored_ssid, ssid, sizeof(stored_ssid));
    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* WiFi power save. The IDF default WIFI_PS_MIN_MODEM sleeps the radio
       between DTIM beacons. Measured on two units, 2026-07-27: no packet loss,
       but ~81ms average LAN round trip and only 18.7 / 75.9 KB/s over HTTP,
       which is what made ~1MB OTA uploads stall and time out.

       Default now: keep MIN_MODEM for the low idle draw, but lift power save
       for the duration of an OTA (wifi_power_save_boost, called around the
       upload), which is the only time bulk throughput matters. Full-rate costs
       roughly 40-80mA continuous, which is a poor trade for a board sitting
       inside a UPS case on a small buck converter, but it is a fine trade for
       the ~30s of an upload.

       CONFIG_WIFI_PS_NONE=y disables power save permanently instead. */

#if CONFIG_WIFI_PS_NONE
    /* Power save off everywhere: full throughput, ~40-80mA extra continuous. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "📶 WiFi power save: OFF (max throughput)");
#else
    ESP_LOGI(TAG, "📶 WiFi power save: MIN_MODEM (boosted during OTA)");
#endif

    ESP_LOGI(TAG, "WiFi initialization complete. Connecting to SSID:%s", ssid);
    return ESP_OK;
}

void wifi_power_save_boost(bool on)
{
#if CONFIG_WIFI_PS_NONE
    (void)on;   /* already off permanently, nothing to toggle */
#else
    esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "📶 WiFi power save %s%s", on ? "OFF (boost)" : "back ON",
             err == ESP_OK ? "" : " [failed]");
#endif
}

esp_err_t wifi_init_sta(const char *ssid, const char *password)
{
    return wifi_connect_sta(ssid, password);
}

esp_err_t wifi_start_portal(void)
{
    ESP_ERROR_CHECK(wifi_manager_init());

    if (s_portal_active) return ESP_OK;

    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);          /* same MAC the MQTT device id uses */
    snprintf(s_portal_ssid, sizeof(s_portal_ssid), "APC-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = { 0 };
    strlcpy((char *)ap_config.ap.ssid, s_portal_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len       = strlen(s_portal_ssid);
    ap_config.ap.channel        = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode       = WIFI_AUTH_OPEN;   /* provisioning portal */

    /* Phase 1: scan the neighbourhood in STA-only mode and cache the list BEFORE
     * the SoftAP serves. There is one radio: scanning once the AP is up drags it
     * off the AP channel for seconds, so the AP is slow to appear and drops the
     * client that triggered the scan. Scan first, save the list, then bring the
     * AP up — by which point it is stable and no more scanning happens. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    do_scan_and_cache();

    /* Phase 2: the AP, now that the list is cached. APSTA rather than AP so the
     * STA netif stays up and a network that comes back on its own is still
     * usable without a power cycle. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    s_portal_active = true;

    esp_netif_ip_info_t ip_info;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        ESP_LOGI(TAG, "🛜  SoftAP \"%s\" up at " IPSTR, s_portal_ssid, IP2STR(&ip_info.ip));
    }

    /* Always the default SoftAP address. Do NOT gate this on esp_netif_get_ip_info():
     * it can report 0.0.0.0 right after start, which silently skipped the DNS task. */
    if (dns_hijack_start(inet_addr(PORTAL_IP_STR)) != ESP_OK) {
        ESP_LOGW(TAG, "DNS hijack failed to start; portal still reachable by IP");
    }

    return ESP_OK;
}

esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "✅ Connected to WiFi SSID:%s", stored_ssid);
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "❌ WiFi association failed (%d retries)", WIFI_MAX_RETRY);
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "❌ WiFi connection timeout");
    return ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_portal_active(void)
{
    return s_portal_active;
}

/* ═══════════════ Cached network scan ═══════════════ */

/* Filled once by wifi_start_portal() before the AP serves, then only read. */
static char s_scan_json[1024] = "[]";

/* Cap on what we pull out of the driver. A dense 2.4GHz neighbourhood sees well
 * past this and the extra records are the weakest ones, useless in a picker. */
#define SCAN_MAX_RECORDS 32

/* Escape for a JSON string literal. An SSID is whatever a stranger nearby chose
 * to broadcast, so a quote or a backslash in one would otherwise break the whole
 * document and leave the portal with an empty picker. */
static void scan_json_escape(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di + 7 < dst_size) {
        unsigned char c = (unsigned char)*src++;
        switch (c) {
            case '"':  dst[di++] = '\\'; dst[di++] = '"';  break;
            case '\\': dst[di++] = '\\'; dst[di++] = '\\'; break;
            default:
                if (c < 0x20) di += snprintf(dst + di, dst_size - di, "\\u%04x", c);
                else          dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

static void do_scan_and_cache(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);   /* blocking */
    if (err != ESP_OK) {
        /* Leaves the cache at "[]"; the portal falls back to manual entry. */
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > SCAN_MAX_RECORDS) num = SCAN_MAX_RECORDS;
    if (num == 0) {
        ESP_LOGW(TAG, "WiFi scan found nothing");
        return;
    }

    wifi_ap_record_t *aps = calloc(num, sizeof(wifi_ap_record_t));
    if (!aps) {
        /* The driver holds the result list until it is read or cleared; skipping
         * both leaks it until the next scan. */
        esp_wifi_clear_ap_list();
        return;
    }
    if (esp_wifi_scan_get_ap_records(&num, aps) != ESP_OK) {
        free(aps);
        return;
    }

    /* The driver already returns these strongest-first, which is what puts the
     * best network at the top of the list and therefore preselected. Duplicates
     * are dropped as they are met, so a mesh or an extender broadcasting one name
     * from several BSSIDs appears once, at its strongest. */
    size_t pos = 1, kept = 0;
    s_scan_json[0] = '[';
    for (uint16_t i = 0; i < num; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (ssid[0] == '\0') continue;              /* hidden, nothing to show */

        char esc[132];                              /* 32 chars * 6 for \u00XX */
        scan_json_escape(esc, ssid, sizeof(esc));

        /* strstr on the accumulated text is enough to spot a repeat: every entry
         * already in the buffer is wrapped in the same quotes. */
        char probe[136];
        snprintf(probe, sizeof(probe), "\"%s\"", esc);
        if (kept && strstr(s_scan_json, probe)) continue;

        size_t need = strlen(probe) + 2;            /* comma + closing bracket */
        if (pos + need >= sizeof(s_scan_json)) break;

        if (kept) s_scan_json[pos++] = ',';
        pos += snprintf(s_scan_json + pos, sizeof(s_scan_json) - pos, "%s", probe);
        kept++;
    }
    s_scan_json[pos++] = ']';
    s_scan_json[pos] = '\0';
    free(aps);

    ESP_LOGI(TAG, "📡 WiFi scan cached: %u networks (%u unique)",
             (unsigned)num, (unsigned)kept);
}

const char *wifi_scan_json(void)
{
    return s_scan_json;
}

const char *wifi_portal_ssid(void)
{
    return s_portal_ssid;
}
