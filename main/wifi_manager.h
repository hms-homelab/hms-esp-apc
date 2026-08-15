#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Max STA association attempts before we give up and fall back to the portal. */
#define WIFI_MAX_RETRY 10

/* Default esp_netif SoftAP address; the portal and DNS hijack both use it. */
#define PORTAL_IP_STR "192.168.4.1"

/* Bring up netifs, the event loop and the WiFi driver. Call once, before either
 * wifi_connect_sta() or wifi_start_portal(). */
esp_err_t wifi_manager_init(void);

/* Join an existing network. Retries WIFI_MAX_RETRY times, then reports failure
 * through wifi_wait_connected(). */
esp_err_t wifi_connect_sta(const char *ssid, const char *password);

/* Blocks until associated + DHCP, or until the retry budget is spent. */
esp_err_t wifi_wait_connected(uint32_t timeout_ms);

/* Open SoftAP ("APC-XXXX") + catch-all DNS so the config page is reachable at
 * http://192.168.4.1/ . Used when there is no saved config, or when joining fails. */
esp_err_t wifi_start_portal(void);

/* Temporarily drop WiFi power save so a bulk transfer runs at full rate.
 * Used around OTA: WIFI_PS_MIN_MODEM sleeps between DTIM beacons, which caps
 * throughput at tens of KB/s and makes a ~1MB upload crawl or time out.
 * Costs roughly 40-80mA while boosted, so keep the window short and always
 * pair the calls. */
void wifi_power_save_boost(bool on);

bool        wifi_is_connected(void);
bool        wifi_portal_active(void);
const char *wifi_portal_ssid(void);

/* Nearby networks as a JSON array of SSID strings, strongest first, one entry per
 * name: ["home","home-guest","neighbour"]. Never NULL; "[]" if the scan found
 * nothing or could not run.
 *
 * The list is captured ONCE by wifi_start_portal(), before the SoftAP begins
 * serving, and this only hands back that cached string. Scanning on demand while
 * the portal is up is the thing to avoid: the radio is shared, so a live scan
 * pulls the AP off its channel for seconds and drops the very client that asked
 * for it. Scan first, serve second. */
const char *wifi_scan_json(void);

/* Kept for source compatibility: init + connect in one call. */
esp_err_t wifi_init_sta(const char *ssid, const char *password);

#endif // WIFI_MANAGER_H
