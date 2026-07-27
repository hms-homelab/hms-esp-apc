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
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>

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

    /* NOTE, deliberate: WiFi power save is left at the IDF default of
       WIFI_PS_MIN_MODEM. The radio therefore sleeps between DTIM beacons, which
       costs latency and bulk throughput. Measured on two units, 2026-07-27:
       0% packet loss on both, but ~81ms average LAN round trip, and 18.7 KB/s
       and 75.9 KB/s respectively for an HTTP download. That is why a ~1MB OTA
       upload can take minutes, and why the HTTP server uses 300s socket
       timeouts instead of the usual few seconds.

       esp_wifi_set_ps(WIFI_PS_NONE) here would raise throughput substantially,
       at the cost of roughly 40-80mA of extra continuous current. It is NOT
       enabled because these boards are commonly mounted inside the UPS case on
       a small buck converter, where the extra constant draw and heat are a
       worse trade than a slow OTA. Revisit only with a thermal measurement. */

    ESP_LOGI(TAG, "WiFi initialization complete. Connecting to SSID:%s", ssid);
    return ESP_OK;
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

    /* APSTA rather than AP: the STA netif stays up so a network that comes back
     * on its own is still usable without a power cycle. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

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

const char *wifi_portal_ssid(void)
{
    return s_portal_ssid;
}
