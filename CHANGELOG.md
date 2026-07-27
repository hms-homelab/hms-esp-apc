# Changelog

## v1.14.1

Decode fixes, all verified against the device's own HID report descriptor rather than assumed layouts.

- Fix permanently-asserted `LB`: the `0x16` PresentStatus bit map was shifted one position, so `BatteryPresent` — set on every healthy UPS that has a battery — was read as low battery. Correct order is Charging, Discharging, ACPresent, BatteryPresent, BelowRemainingCapacityLimit, ShutdownImminent, RemainingTimeLimitExpired, CommunicationLost, NeedReplacement, Overload, VoltageNotRegulated. Read as 16-bit so NeedReplacement and Overload are reachable at all
- Fix `0x06`: it is three byte-wide fields (Charging, Discharging, APCStatusFlag), not a bitfield. The old code read the vendor byte as status bits, so `0x06` and `0x16` fought over the same flags and the status string flapped between `OL` and `OL CHRG LB`
- Unswap beeper and self-test: `0x18` is `AudibleAlarmControl`, `0x21` is `Test`. Self-test previously reported "Test passed" on a UPS that had never run one
- Fix `0x36`: it is `APCLineFailCause` (the last transfer reason), not input frequency — this UPS exposes no frequency usage anywhere
- Fix `0x13` (`ACPresent`, was published as `delay_reboot=1`) and `0x14` (`BelowRemainingCapacityLimit` + `ShutdownImminent`, was read as a shutdown delay)
- Add `0x40`/`0x41`, the real `APCDelayBeforeReboot` / `APCDelayBeforeShutdown`, and poll them
- Fix `0x17`: `RemainingTimeLimit`, not a reboot timer. Drop the `reboot_timer` sensor — this UPS declares no `DelayBeforeReboot` usage
- Stop publishing `0x10` and `0x12` (`CapacityGranularity2`/`1`) as beeper status and runtime threshold
- Decode manufacture dates with the packed HID/SBS format (`(year-1980)<<9 | month<<5 | day`); `battery_mfr_date` now reads `2022/05/26` instead of `21690 days`
- Read identity from USB string descriptors, which the HID report descriptor only references by index: manufacturer, model, serial, firmware revision, and battery chemistry. `battery_type` was reporting `NiMH` on a lead-acid UPS because the string index (4) was being decoded as a chemistry enum; it now reads `PbAc`
- Strip APC's space padding and split the firmware revision out of `iProduct` (`"Back-UPS XS 1000M FW:945.d11 .D USB FW:"`)
- Use the real manufacturer and model in the Home Assistant discovery payload instead of hardcoded strings; publish `firmware_version` and `ups_serial`
- Add `hid_debug.c/.h` and `GET /hid`: fetches the HID report descriptor at enumeration and keeps the last raw payload of every report ID, tagged interrupt vs feature. These boards have no serial console once wired to the UPS — their native USB is the host link — so this is the only way to see what the device actually sends

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
