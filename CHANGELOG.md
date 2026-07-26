# Changelog

## v1.14.0

- Add SoftAP config portal with a catch-all DNS hijacker (`dns_hijack.c`): open AP `APC-XXXX` at `192.168.4.1`, any hostname resolves to the config page
- Portal comes up in two cases: no config saved in NVS, or the saved network refuses association after `WIFI_MAX_RETRY` (10) attempts
- Replace the reboot-loop on WiFi failure — an unreachable or renamed network no longer needs a USB reflash to recover
- Track whether a real SSID was ever saved (`app_config_t.provisioned`) so a blank NVS boots straight to the portal instead of trying the compiled-in Kconfig SSID
- Split WiFi bring-up into `wifi_manager_init()` / `wifi_connect_sta()` / `wifi_start_portal()`; both netifs are created up front so the AP can be raised without restarting the driver
- Fix POST `/save` failing from a browser with 431 "header too long": raise `CONFIG_HTTPD_MAX_REQ_HDR_LEN` from the 512-byte default to 2048 (curl worked, browsers did not)
- Change the default publish interval to 60s in `sdkconfig.defaults`, which was still overriding the 60s Kconfig default with 10s

## v1.13.0

- Add OTA firmware updates over HTTP: "Firmware Update" card on the web UI uploads a `.bin` (POST `/ota`) via `esp_ota`, writes the inactive slot, verifies, sets boot, and reboots
- Switch to 4MB flash with a dual-OTA (A/B) partition table (`partitions.csv`: nvs, otadata, phy_init, ota_0, ota_1)
- Single source of truth for the firmware version (`version.h`), shown on the config page
- Note: the first OTA-capable image must be flashed over USB once per board to lay down the A/B partition table; subsequent updates are over-the-air

## v1.12.0

- Add editable Device Identity in the web config UI: friendly name and device ID slug
- Friendly name sets the Home Assistant device display name (was hardcoded `APC UPS (MAC)`)
- Device ID slug overrides the MAC-derived id (`apc_ups_<slug>`); blank keeps the MAC default
- Persist both to NVS; sanitize slug to MQTT-safe chars and strip JSON-breaking chars from the name

## v1.11.0

- Add web configuration UI at `/` for WiFi, MQTT, and publish interval settings
- Add live status and serial logs page at `/status` with auto-refresh
- Add NVS-based config persistence (survives reboots, Kconfig defaults as fallback)
- Refactor `wifi_init_sta()` and `mqtt_init()` to accept runtime parameters
- Change default publish interval from 10s to 60s

## v1.10.0

- Switch to DHCP for multi-device support

## v1.9.0

- Fix battery low charge threshold and remove unavailable sensors

## v1.7.1

- Fix unknown metrics and add missing sensors
