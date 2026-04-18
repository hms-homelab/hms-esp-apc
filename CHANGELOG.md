# Changelog

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
