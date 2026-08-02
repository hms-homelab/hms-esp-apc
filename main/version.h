#ifndef VERSION_H
#define VERSION_H

/* Single source of truth for the firmware version. Read by main.c and the web UI,
 * parsed by CMakeLists.txt for PROJECT_VER (the app descriptor OTA reads), and
 * checked against the pushed tag by .github/workflows/release.yml. Keep the line
 * below in this exact shape; those parsers match on it. */
#define HMS_ESP_APC_VERSION "1.15.1"

#endif // VERSION_H
