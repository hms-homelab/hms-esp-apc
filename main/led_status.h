#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/* Status LED. Pin and driver are Kconfig options; both can also be overridden
 * at runtime through GET /led so a board with the LED on an unexpected pin can
 * be identified without reflashing. */

typedef enum {
    LED_STATE_WIFI_DOWN = 0,  /* slow red pulse: connecting, or portal is up */
    LED_STATE_WIFI_UP,        /* solid red: joined the network */
} led_state_t;

esp_err_t led_status_init(void);
void      led_status_set(led_state_t state);

/* Runtime probing. */
esp_err_t   led_status_reinit(int gpio, bool ws2812);  /* rebind to another pin */
void        led_status_test(uint8_t r, uint8_t g, uint8_t b); /* hold a colour */
void        led_status_auto(void);                     /* resume the state machine */
const char *led_status_info(void);                     /* pin, driver, last error */

/* Drive every safe GPIO in [from,to] high (or low) at once, for binary-searching
 * which pin an unknown indicator LED sits on. Unsafe pins (USB, flash/PSRAM,
 * strapping) are skipped; returns a description of what was actually driven. */
const char *led_status_sweep(int from, int to, int level);

#endif // LED_STATUS_H
