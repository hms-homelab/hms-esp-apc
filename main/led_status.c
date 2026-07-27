#include "led_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include <stdio.h>

static const char *TAG = "led_status";

#define BLINK_ON_MS    1000
#define BLINK_OFF_MS   1000

/* Dim on purpose: these WS2812s are painfully bright at full scale, and this
 * sits on a UPS that probably lives in a bedroom or an office corner. */
#define RED_R 40
#define RED_G 0
#define RED_B 0

static volatile led_state_t s_state     = LED_STATE_WIFI_DOWN;
static volatile bool        s_test_mode = false;
static uint8_t  s_test_r = 0, s_test_g = 0, s_test_b = 0;

static led_strip_handle_t s_strip = NULL;
static int   s_gpio   = CONFIG_STATUS_LED_GPIO;
static bool  s_ws2812 = CONFIG_STATUS_LED_WS2812;
static char  s_info[128] = "uninitialised";

/* Guards s_strip. /led can rebind to another pin from the HTTP task while the
 * LED task is mid-refresh; without this the handle is freed underneath it and
 * the httpd worker wedges. */
static SemaphoreHandle_t s_lock = NULL;

#define LED_LOCK()   do { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); } while (0)
#define LED_UNLOCK() do { if (s_lock) xSemaphoreGive(s_lock); } while (0)

static void led_write_rgb_locked(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_ws2812) {
        if (!s_strip) return;
        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);
    } else {
        bool on = (r || g || b);
#if CONFIG_STATUS_LED_ACTIVE_LOW
        gpio_set_level(s_gpio, on ? 0 : 1);
#else
        gpio_set_level(s_gpio, on ? 1 : 0);
#endif
    }
}

static void led_write_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    LED_LOCK();
    led_write_rgb_locked(r, g, b);
    LED_UNLOCK();
}

static void led_write(bool on)
{
    if (on) led_write_rgb(RED_R, RED_G, RED_B);
    else    led_write_rgb(0, 0, 0);
}

/* Bring the LED up on the given pin. Safe to call repeatedly: any previous
 * RMT channel is released first. */
static esp_err_t led_hw_init(int gpio, bool ws2812)
{
    LED_LOCK();
    if (s_strip) {
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    /* Return the old pin to a known state before moving on. */
    gpio_reset_pin(s_gpio);

    s_gpio   = gpio;
    s_ws2812 = ws2812;

    if (ws2812) {
        led_strip_config_t strip_cfg = {
            .strip_gpio_num   = gpio,
            .max_leds         = 1,
            .led_model        = LED_MODEL_WS2812,
            .led_pixel_format = LED_PIXEL_FORMAT_GRB,
            .flags.invert_out = false,
        };
        led_strip_rmt_config_t rmt_cfg = {
            .clk_src        = RMT_CLK_SRC_DEFAULT,
            .resolution_hz  = 10 * 1000 * 1000,
            .flags.with_dma = false,
        };
        esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
        if (err != ESP_OK) {
            snprintf(s_info, sizeof(s_info), "WS2812 GPIO%d FAILED: %s",
                     gpio, esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", s_info);
            LED_UNLOCK();
            return err;
        }
        led_strip_clear(s_strip);
        snprintf(s_info, sizeof(s_info), "WS2812 on GPIO%d, ok", gpio);
    } else {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        snprintf(s_info, sizeof(s_info), "plain GPIO%d, ok", gpio);
    }

    ESP_LOGI(TAG, "💡 Status LED: %s", s_info);
    LED_UNLOCK();
    return ESP_OK;
}

static void led_task(void *arg)
{
    bool phase = false;

    while (1) {
        if (s_test_mode) {
            led_write_rgb(s_test_r, s_test_g, s_test_b);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        switch (s_state) {
        case LED_STATE_WIFI_UP:
            led_write(true);                       /* solid */
            vTaskDelay(pdMS_TO_TICKS(200));
            break;

        case LED_STATE_WIFI_DOWN:
        default:
            phase = !phase;
            led_write(phase);
            vTaskDelay(pdMS_TO_TICKS(phase ? BLINK_ON_MS : BLINK_OFF_MS));
            break;
        }
    }
}

esp_err_t led_status_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_err_t err = led_hw_init(CONFIG_STATUS_LED_GPIO, CONFIG_STATUS_LED_WS2812);
    led_write(false);

    if (xTaskCreate(led_task, "led_status", 2560, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create LED task");
        return ESP_ERR_NO_MEM;
    }
    return err;
}

void led_status_set(led_state_t state)
{
    s_state = state;
}

esp_err_t led_status_reinit(int gpio, bool ws2812)
{
    return led_hw_init(gpio, ws2812);
}

void led_status_test(uint8_t r, uint8_t g, uint8_t b)
{
    s_test_r = r; s_test_g = g; s_test_b = b;
    s_test_mode = true;
}

void led_status_auto(void)
{
    s_test_mode = false;
}

const char *led_status_info(void)
{
    return s_info;
}

/* Pins we must not touch on this board:
 *   0, 3, 45, 46  strapping
 *   19, 20        USB D-/D+, in use for the UPS host link
 *   26..32        SPI flash / PSRAM
 *   43, 44        UART0 TX/RX
 * Everything else on an ESP32-S3 is fair game as a plain output. */
static bool pin_is_safe(int p)
{
    if (p < 0 || p > 48)                return false;
    if (p == 0 || p == 3 || p == 45 || p == 46) return false;
    if (p == 19 || p == 20)             return false;
    if (p >= 26 && p <= 32)             return false;
    if (p == 43 || p == 44)             return false;
    return true;
}

static char s_sweep[256];

const char *led_status_sweep(int from, int to, int level)
{
    if (from > to) { int t = from; from = to; to = t; }

    /* The status LED task would fight us for its own pin. */
    s_test_mode = true;

    int  n = 0;
    char list[192] = {0};
    size_t used = 0;

    for (int p = from; p <= to; p++) {
        if (!pin_is_safe(p)) continue;
        if (p == s_gpio && s_ws2812) continue;   /* leave the RMT pin bound */

        gpio_config_t io = {
            .pin_bit_mask = 1ULL << p,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) != ESP_OK) continue;
        gpio_set_level(p, level ? 1 : 0);

        n++;
        if (used < sizeof(list) - 8)
            used += snprintf(list + used, sizeof(list) - used, "%d ", p);
    }

    snprintf(s_sweep, sizeof(s_sweep), "drove %d pin(s) %s in [%d..%d]: %s\n",
             n, level ? "HIGH" : "LOW", from, to, list);
    ESP_LOGI(TAG, "%s", s_sweep);
    return s_sweep;
}
