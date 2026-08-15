# Changelog

## v1.16.0

- **The config portal picks the network from a list.** The SSID was a free-text
  box, so onboarding meant typing a network name by hand on a phone, exactly,
  from memory. It is now a dropdown that fills itself when the page loads, with
  the strongest network already selected. An **Other network...** entry reveals a
  text box, because a hidden network still has to be typed
- The scan runs **once, in STA-only mode, before the SoftAP starts serving**, and
  the result is cached. There is one radio: scanning after the access point is up
  drags it off its channel for seconds, so the AP appears slowly and drops the
  client that triggered the scan. `GET /scan` is a read of that cache and returns
  immediately
- A saved SSID that is in range wins over the strongest one. Without that, simply
  opening the page and pressing Save would move an already-configured board onto
  whichever neighbouring network happened to be loudest
- **The portal is ordered WiFi, MQTT, then everything else.** WiFi first because
  on a fresh board it is the only card that matters and the rest needed
  scrolling past on a phone; Device Identity moved below both, as its defaults
  are MAC-derived and work untouched
- Raised `max_uri_handlers` from 8 to 12. Seven routes plus the portal catch-all
  had it sitting exactly on the limit, where one more route would have failed to
  register at start-up and simply 404'd with nothing in the log to say why

## v1.15.1

- **`/hid` reported a stale VID:PID after the UPS was unplugged.** `DEV_GONE`
  cleared `attached_is_power_device` but not the IDs, so the page showed the
  departed device's `051D:0002` next to `power device = no`. That reads as "the
  attached device is not a UPS" when it means "nothing is attached", and it sent
  a debugging session chasing a regression that did not exist. Now clears both
  and prints `(nothing attached to the USB host port)`
- Shadow decode validated live on hardware for the first time, against a
  Back-UPS XS 1000M (serial 0B2222N10328): all 12 compared rows agree, zero
  DIFF. `battery_charge` 100%, `battery_runtime` 2224s, `battery_voltage`
  13.71V, `input_voltage` 119V, `load_percent` 16%, `nominal_power` 600W, and
  all six status flags. This is a second physical unit from the one whose
  descriptor was captured for the replay tests, and it parses to the same 105
  fields

## v1.15.0

Decode is now driven by the device's own HID report descriptor instead of bit
positions reverse-engineered from one specific APC. **Shadow mode: MQTT still
publishes the hand-written parser's numbers.** The new path runs alongside it and
is visible on `/hid` for comparison; nothing published has changed yet.

- **New `hid_pdc.c`, a HID report descriptor walker.** Parses the descriptor into
  a field table of (report ID, usage path, bit offset, bit size, logical range,
  unit exponent). Report IDs are assigned arbitrarily by each vendor's firmware,
  so "report `0x16` is PresentStatus" was only ever true of the one APC it was
  read off. No ESP-IDF dependencies, so it compiles natively for tests
- **New `ups_map.c`, usage path to metrics.** Matches on (ancestor collection,
  leaf usage), because `Voltage` (`0x84:0x30`) means battery voltage under
  PowerSummary, mains under Input and output under Output. Equivalent to NUT's
  hid2nut tables
- Scaling now comes from the descriptor's declared unit exponent rather than the
  magic `/100.0f` and `/10.0f` divisors scattered through `apc_hid_parser.c`
- **Scaling also applies the Unit field's dimensional scale, which is not
  optional.** HID's SI Linear system is centimetre-gram-second, so a volt in HID
  units is `10^-7` V. The deployed Back-UPS XS 1000M declares Unit Exponent 5 on
  its battery voltage and expects the reader to add `-7`; that is how raw 1371
  becomes 13.71 V. Applying the exponent alone read it as 137100000 V. Caught by
  replaying the real 1049-byte descriptor off a live board, which is precisely
  what a hand-written test descriptor cannot expose, since writing one means
  choosing the exponent yourself
- Identity strings resolve through the string index the descriptor names, rather
  than a hardcoded index table. Decoding that index as an enum is what once
  reported `NiMH` on a lead-acid UPS
- **The VID/PID gate is gone.** Admission is now "exposes a HID interface", and
  the real test is whether the descriptor declares Usage Page `0x84` (Power
  Device). Any UPS that implements the class correctly is decoded with no new
  code, APC and Tripp Lite alike. A PID table never could answer that question
- **`poll_reports[]` is no longer hardcoded.** The Feature report IDs to poll are
  derived from what this device actually declares and this firmware actually
  maps. The old list remains as a fallback if the descriptor cannot be read
- `/hid` gains the parsed field table, a side-by-side of both decodes with `DIFF`
  markers, and the attached VID:PID with whether it is a Power Device at all.
  That last part identifies unknown hardware without needing a NUT box: a device
  that enumerates as HID but declares no `0x84` page is usually a USB-to-serial
  bridge speaking Megatec/Q1 ASCII, which this firmware cannot decode and now
  releases instead of polling forever
- `test/extract_hid.py` + `test/hid_replay.c` replay a captured `/hid` page (real
  descriptor, real report bytes) through the new decode on the host. Validated
  against a live Back-UPS XS 1000M: charge 98%, runtime 18763s, battery 13.71V,
  nominal 12.00V, input 121V, 600W, transfer 88/139V, and all eight status flags
  match what that same board's hand-written parser reported
- Host test harness in `test/hid_pdc_test.c`, 107 assertions, no board required:
  `cc -Wall -Wextra -I../main -o /tmp/t hid_pdc_test.c ../main/hid_pdc.c ../main/ups_map.c`
  Covers the exact PresentStatus bit order v1.14.1 arrived at by hand, plus what
  hand-decoding never had to handle: sign extension, constant padding that must
  advance the bit cursor without being stored, per-report cursor isolation, and
  Push/Pop. Includes 199 truncations and 3168 byte mutations of the descriptor,
  clean under `-fsanitize=address,undefined` (a malformed descriptor must not
  crash a board whose USB port is the UPS link and therefore has no console)

## v1.14.3

- **Fix OTA rebooting the device mid-upload.** `esp_ota_begin()` was called with
  `OTA_SIZE_UNKNOWN`, which erases the *entire* partition. `ota_0` is `0x1E0000`
  (1.875 MB), and erasing more than ~1280K takes over 5s, tripping the default
  task watchdog (espressif/esp-idf#578). Passing the real `content_len` erases
  only the sectors needed
- The failure looked like a network timeout and was not: the client keeps
  filling TCP buffers while the device is blocked in the erase, so uploads died
  around 130 KB after ~20s instead of at byte zero. Confirmed it was a reboot by
  watching the `/status` log ring go from 75 lines to 0 across an attempt
- Retrying did not help, because every attempt restarts the full erase
- Lift WiFi power save for the duration of an OTA (`wifi_power_save_boost`) and
  restore it after. The IDF default `WIFI_PS_MIN_MODEM` sleeps the radio between
  DTIM beacons, which held uploads to ~6 KB/s. Measured after: **61.7 KB/s, a
  full 1 MB image in 17s on the first attempt**
- Add `CONFIG_WIFI_PS_NONE` to disable power save permanently, for mains-powered
  boards that want low latency and can afford ~40-80mA continuous
- OTA read buffer 2048 -> 4096, matching the flash sector size


## v1.14.2

- Add a status LED: solid red once WiFi is joined, slow red pulse (1s on / 1s off) while it is not. In practice the LED is initialised after the 10s boot delay and a normal join takes a couple of seconds, so what you see is dark, then solid; the pulse is really only visible when a join is failing or the config portal is up, which is when it matters
- The LED is the onboard WS2812. This board is an **ESP32-S3-Zero**, so it sits on **GPIO21** (the ESP32-S3 SuperMini's GPIO48 is a different board and lights nothing here)
- Pin and driver are Kconfig options (`STATUS_LED_GPIO`, `STATUS_LED_WS2812`, `STATUS_LED_ACTIVE_LOW`, `STATUS_LED_ENABLED`) so other boards can point elsewhere or compile it out
- Add `GET /led` to rebind and test the LED at runtime: `?gpio=21&ws=1&r=255&g=0&b=0` holds a colour, `?from=1&to=48&level=1` drives a range of pins for locating an unknown indicator, `?auto=1` resumes the status pattern. These boards lose their serial console to USB host mode, so finding an LED pin would otherwise cost a reflash per guess
- Note the range sweep only finds plain GPIO LEDs; an addressable WS2812 ignores a static level and needs the RMT timing, so use `?gpio=N&ws=1` to test those
- Guard the strip handle with a mutex: rebinding from the HTTP task while the LED task was mid-refresh freed the handle underneath it and wedged the httpd worker, leaving the board pingable but serving nothing (and therefore un-OTA-able)
- Add the `espressif/led_strip` dependency and commit `dependencies.lock`; ignore `managed_components/`

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
