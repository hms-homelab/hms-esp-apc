#include "http_server.h"
#include "apc_hid_parser.h"
#include "hid_debug.h"
#include "usb_host_manager.h"
#include "wifi_manager.h"
#include "led_status.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "http_server";

/* ═══════════════ Log Ring Buffer ═══════════════ */
#define LOG_RING_SIZE 80
#define LOG_LINE_LEN  200

static char log_ring[LOG_RING_SIZE][LOG_LINE_LEN];
static int  log_write_idx = 0;
static int  log_count     = 0;
static SemaphoreHandle_t log_mutex = NULL;
static vprintf_like_t    original_vprintf_fn = NULL;

static app_config_t *current_config = NULL;

/* Drop ANSI CSI sequences (ESP_LOG colour codes) in place. The serial console
   renders them; the web log view would print them as literal "[0;32m". */
static void strip_ansi(char *s)
{
    char *w = s;
    for (const char *r = s; *r; ) {
        if (r[0] == '\033' && r[1] == '[') {
            r += 2;
            while (*r && (*r < '@' || *r > '~')) r++;  /* parameter + intermediate bytes */
            if (*r) r++;                                /* final byte */
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static int capture_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        vsnprintf(log_ring[log_write_idx], LOG_LINE_LEN, fmt, copy);
        strip_ansi(log_ring[log_write_idx]);
        int len = strlen(log_ring[log_write_idx]);
        if (len > 0 && log_ring[log_write_idx][len - 1] == '\n')
            log_ring[log_write_idx][len - 1] = '\0';
        log_write_idx = (log_write_idx + 1) % LOG_RING_SIZE;
        if (log_count < LOG_RING_SIZE) log_count++;
        xSemaphoreGive(log_mutex);
    }
    va_end(copy);

    if (original_vprintf_fn)
        return original_vprintf_fn(fmt, args);
    return vprintf(fmt, args);
}

/* ═══════════════ URL Decode / Form Parse ═══════════════ */

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di < dst_size - 1) {
        if (*src == '+') {
            dst[di++] = ' '; src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

static bool get_form_value(const char *body, const char *key, char *value, size_t value_size)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *start = strstr(body, search);
    if (!start) return false;

    /* make sure we matched the full key, not a suffix of another key */
    if (start != body && *(start - 1) != '&') {
        /* search again after first hit */
        start = strstr(start + 1, search);
        if (!start) return false;
    }

    start += strlen(search);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    char encoded[256];
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(value, encoded, value_size);
    return true;
}

/* Sanitize an MQTT-topic-safe slug: lowercase, keep [a-z0-9_-], drop the rest. */
static void sanitize_slug(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (; *src && di < dst_size - 1; src++) {
        char c = *src;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            dst[di++] = c;
    }
    dst[di] = '\0';
}

/* Sanitize a JSON-safe display name: strip quotes, backslashes and control chars. */
static void sanitize_name(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    for (; *src && di < dst_size - 1; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\' || c < 0x20) continue;
        dst[di++] = (char)c;
    }
    dst[di] = '\0';
}

/* ═══════════════ Config Load / Save (NVS) ═══════════════ */

esp_err_t config_load(app_config_t *config)
{
    /* Kconfig defaults */
    strlcpy(config->wifi_ssid, CONFIG_WIFI_SSID,       sizeof(config->wifi_ssid));
    strlcpy(config->wifi_pass, CONFIG_WIFI_PASSWORD,    sizeof(config->wifi_pass));
    strlcpy(config->mqtt_url,  CONFIG_MQTT_BROKER_URL,  sizeof(config->mqtt_url));
    strlcpy(config->mqtt_user, CONFIG_MQTT_USERNAME,     sizeof(config->mqtt_user));
    strlcpy(config->mqtt_pass, CONFIG_MQTT_PASSWORD,     sizeof(config->mqtt_pass));
    config->publish_interval_ms = CONFIG_MQTT_PUBLISH_INTERVAL_MS;
    config->device_slug[0] = '\0';   /* blank => MAC-derived id */
    config->device_name[0] = '\0';   /* blank => "APC UPS (MAC)" */
    config->provisioned = false;

    /* Override from NVS if previously saved */
    nvs_handle_t nvs;
    if (nvs_open("config", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(config->wifi_ssid);
        if (nvs_get_str(nvs, "wifi_ssid", config->wifi_ssid, &len) == ESP_OK &&
            config->wifi_ssid[0] != '\0') {
            config->provisioned = true;   /* a real SSID was saved at some point */
        }
        len = sizeof(config->wifi_pass);  nvs_get_str(nvs, "wifi_pass", config->wifi_pass, &len);
        len = sizeof(config->mqtt_url);   nvs_get_str(nvs, "mqtt_url",  config->mqtt_url,  &len);
        len = sizeof(config->mqtt_user);  nvs_get_str(nvs, "mqtt_user", config->mqtt_user, &len);
        len = sizeof(config->mqtt_pass);  nvs_get_str(nvs, "mqtt_pass", config->mqtt_pass, &len);
        nvs_get_u32(nvs, "pub_interval", &config->publish_interval_ms);
        len = sizeof(config->device_slug); nvs_get_str(nvs, "dev_slug", config->device_slug, &len);
        len = sizeof(config->device_name); nvs_get_str(nvs, "dev_name", config->device_name, &len);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Config loaded from NVS (overrides applied)");
    } else {
        ESP_LOGI(TAG, "No NVS config found, using Kconfig defaults");
    }

    return ESP_OK;
}

static esp_err_t config_save(const app_config_t *config)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, "wifi_ssid", config->wifi_ssid);
    nvs_set_str(nvs, "wifi_pass", config->wifi_pass);
    nvs_set_str(nvs, "mqtt_url",  config->mqtt_url);
    nvs_set_str(nvs, "mqtt_user", config->mqtt_user);
    nvs_set_str(nvs, "mqtt_pass", config->mqtt_pass);
    nvs_set_u32(nvs, "pub_interval", config->publish_interval_ms);
    nvs_set_str(nvs, "dev_slug", config->device_slug);
    nvs_set_str(nvs, "dev_name", config->device_name);

    err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

/* ═══════════════ HTML Helpers ═══════════════ */

static void html_escape(char *dst, const char *src, size_t dst_size)
{
    size_t di = 0;
    while (*src && di < dst_size - 6) {
        switch (*src) {
            case '<':  di += snprintf(dst + di, dst_size - di, "&lt;");   break;
            case '>':  di += snprintf(dst + di, dst_size - di, "&gt;");   break;
            case '&':  di += snprintf(dst + di, dst_size - di, "&amp;");  break;
            case '"':  di += snprintf(dst + di, dst_size - di, "&quot;"); break;
            default:   dst[di++] = *src;
        }
        src++;
    }
    dst[di] = '\0';
}

static const char *PAGE_STYLE =
    "body{font-family:sans-serif;max-width:700px;margin:0 auto;padding:20px;background:#1a1a2e;color:#e0e0e0}"
    "h1{color:#4db8ff;text-align:center}h2{color:#a0a0c0;border-bottom:1px solid #333;padding-bottom:5px}"
    "input,select{width:100%;padding:8px;margin:5px 0 15px;box-sizing:border-box;"
        "background:#16213e;color:#e0e0e0;border:1px solid #333;border-radius:4px}"
    "label{font-weight:bold;color:#a0a0c0}"
    "button{background:#0f3460;color:white;padding:12px 24px;border:none;border-radius:4px;"
        "cursor:pointer;width:100%;font-size:16px}"
    "button:hover{background:#1a508b}"
    ".card{background:#16213e;padding:15px;border-radius:8px;margin:15px 0}"
    "table{width:100%;border-collapse:collapse}"
    "td,th{padding:6px 10px;text-align:left;border-bottom:1px solid #333}"
    "th{color:#a0a0c0;width:40%}.val{color:#4db8ff;font-weight:bold}"
    "pre{background:#0d1117;color:#c9d1d9;padding:12px;border-radius:6px;"
        "overflow-x:auto;font-size:12px;max-height:500px;overflow-y:auto;line-height:1.4}"
    "a{color:#4db8ff;text-decoration:none}a:hover{text-decoration:underline}"
    ".nav{text-align:center;margin:10px 0}"
    ".online{color:#4caf50}.offline{color:#f44336}";

static const char *PAGE_NAV =
    "<div class='nav'><a href='/'>Config</a> | <a href='/status'>Status &amp; Logs</a> | "
    "<a href='/hid'>Raw HID</a></div>";

/* ═══════════════ GET / — Config Page ═══════════════ */

static void send_page_header(httpd_req_t *req, const char *title, bool auto_refresh)
{
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>");
    if (auto_refresh) {
        httpd_resp_sendstr_chunk(req, "<meta http-equiv='refresh' content='5'>");
    }
    httpd_resp_sendstr_chunk(req, "<title>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</title><style>");
    httpd_resp_sendstr_chunk(req, PAGE_STYLE);
    httpd_resp_sendstr_chunk(req, "</style></head><body><h1>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</h1>");
    httpd_resp_sendstr_chunk(req, PAGE_NAV);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char buf[512];

    send_page_header(req, "APC UPS Bridge", false);

    char ssid_esc[128], url_esc[256], user_esc[128], slug_esc[64], name_esc[128];
    html_escape(ssid_esc, current_config->wifi_ssid, sizeof(ssid_esc));
    html_escape(url_esc,  current_config->mqtt_url,  sizeof(url_esc));
    html_escape(user_esc, current_config->mqtt_user,  sizeof(user_esc));
    html_escape(slug_esc, current_config->device_slug, sizeof(slug_esc));
    html_escape(name_esc, current_config->device_name, sizeof(name_esc));

    httpd_resp_sendstr_chunk(req, "<form method='POST' action='/save'>");

    /* Device Identity */
    char idbuf[768];
    snprintf(idbuf, sizeof(idbuf),
        "<div class='card'><h2>Device Identity</h2>"
        "<label>Friendly Name</label>"
        "<input name='device_name' maxlength='63' placeholder='APC UPS (MAC)' value='%s'>"
        "<label>Device ID slug</label>"
        "<input name='device_slug' maxlength='23' placeholder='(MAC-derived)' value='%s'>"
        "<small style='color:#8080a0'>MQTT id becomes "
        "<b>apc_ups_&lt;slug&gt;</b>. Leave blank to use the MAC address. "
        "a-z 0-9 _ - only. Changing this makes a new Home Assistant device.</small>"
        "</div>",
        name_esc, slug_esc);
    httpd_resp_sendstr_chunk(req, idbuf);

    /* WiFi. Stored passwords are never sent back to the browser: this page is
       unauthenticated, so anything rendered here is readable by anyone on the
       LAN. A blank field on save means "keep the stored one" (see save_handler). */
    snprintf(buf, sizeof(buf),
        "<div class='card'><h2>WiFi</h2>"
        "<label>SSID</label><input name='wifi_ssid' value='%s'>"
        "<label>Password</label>"
        "<input name='wifi_pass' type='password' placeholder='%s' autocomplete='new-password'>"
        "<label style='font-weight:normal'>"
        "<input type='checkbox' name='wifi_pass_clear' value='1' style='width:auto'> "
        "Clear the saved password</label>"
        "</div>",
        ssid_esc,
        current_config->wifi_pass[0] ? "unchanged, type to replace" : "(none set)");
    httpd_resp_sendstr_chunk(req, buf);

    /* MQTT */
    httpd_resp_sendstr_chunk(req, "<div class='card'><h2>MQTT</h2>");
    snprintf(buf, sizeof(buf),
        "<label>Broker URL</label><input name='mqtt_url' value='%s'>", url_esc);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<label>Username</label><input name='mqtt_user' value='%s'>", user_esc);
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf),
        "<label>Password</label>"
        "<input name='mqtt_pass' type='password' placeholder='%s' autocomplete='new-password'>"
        "<label style='font-weight:normal'>"
        "<input type='checkbox' name='mqtt_pass_clear' value='1' style='width:auto'> "
        "Clear the saved password</label>",
        current_config->mqtt_pass[0] ? "unchanged, type to replace" : "(none set)");
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, "</div>");

    /* Interval */
    snprintf(buf, sizeof(buf),
        "<div class='card'><h2>Publish Interval</h2>"
        "<label>Seconds</label>"
        "<input name='interval' type='number' min='5' max='300' value='%lu'>"
        "</div>",
        (unsigned long)(current_config->publish_interval_ms / 1000));
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req,
        "<button type='submit'>Save &amp; Reboot</button></form>");

    /* Firmware Update (OTA over HTTP) — its own action, outside the config form */
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>Firmware Update</h2>"
        "<p>Running firmware: <span class='val'>v" HMS_ESP_APC_VERSION "</span></p>"
        "<label>Select .bin</label>"
        "<input type='file' id='fw' accept='.bin'>"
        "<button type='button' onclick='doOta()'>Upload &amp; Flash</button>"
        "<p id='otastat' style='color:#a0a0c0'></p></div>"
        "<script>"
        "function doOta(){"
        "var f=document.getElementById('fw').files[0];"
        "if(!f){alert('Pick a .bin file first');return;}"
        "var s=document.getElementById('otastat');"
        "s.textContent='Uploading '+f.name+' ('+f.size+' bytes)... do not power off.';"
        "fetch('/ota',{method:'POST',body:f})"
        ".then(function(r){return r.text().then(function(t){return {ok:r.ok,t:t};});})"
        ".then(function(o){s.textContent=o.ok?(o.t+' Rebooting, reconnect in ~20s.'):('Failed: '+o.t);})"
        ".catch(function(e){s.textContent='Upload failed: '+e;});"
        "}"
        "</script>");

    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ═══════════════ POST /ota — Firmware Update (esp_ota) ═══════════════ */

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ESP_LOGE(TAG, "OTA: no update partition (single-app layout?)");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: writing to partition '%s' (size %lu), incoming %d bytes",
             update->label, (unsigned long)update->size, req->content_len);

    if (req->content_len <= 0 || (size_t)req->content_len > update->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad image size");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(2048);
    if (!buf) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 2048 ? remaining : 2048);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;   /* retry on transient timeout */
        if (r <= 0) {
            ESP_LOGE(TAG, "OTA: recv error (%d) with %d bytes left", r, remaining);
            free(buf);
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota, buf, r) != ESP_OK) {
            free(buf);
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write failed");
            return ESP_FAIL;
        }
        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end failed: %s (bad/corrupt image)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid image");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success, booting '%s' on restart", update->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK, firmware flashed.");

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* ═══════════════ GET /status — Metrics + Logs ═══════════════ */

static esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    char buf[512];

    send_page_header(req, "APC UPS Status", true);

    /* UPS Metrics */
    const ups_metrics_t *m = apc_hid_get_metrics();

    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>UPS Metrics</h2><table>");

    if (m->valid) {
        snprintf(buf, sizeof(buf),
            "<tr><th>Status</th><td class='val %s'>%s</td></tr>"
            "<tr><th>Battery Charge</th><td class='val'>%.0f%%</td></tr>"
            "<tr><th>Battery Voltage</th><td class='val'>%.1f V</td></tr>"
            "<tr><th>Battery Runtime</th><td class='val'>%.0f s (%.1f min)</td></tr>",
            m->status.online ? "online" : "offline",
            m->status_string,
            m->battery_charge,
            m->battery_voltage,
            m->battery_runtime, m->battery_runtime / 60.0f);
        httpd_resp_sendstr_chunk(req, buf);

        snprintf(buf, sizeof(buf),
            "<tr><th>Input Voltage</th><td class='val'>%.0f V</td></tr>"
            "<tr><th>Load</th><td class='val'>%.0f%%</td></tr>",
            m->input_voltage, m->load_percent);
        httpd_resp_sendstr_chunk(req, buf);

        if (m->nominal_power > 0) {
            snprintf(buf, sizeof(buf),
                "<tr><th>Nominal Power</th><td class='val'>%.0f W</td></tr>",
                m->nominal_power);
            httpd_resp_sendstr_chunk(req, buf);
        }
        if (m->input_voltage_nominal > 0) {
            snprintf(buf, sizeof(buf),
                "<tr><th>Nominal Input</th><td class='val'>%.0f V</td></tr>",
                m->input_voltage_nominal);
            httpd_resp_sendstr_chunk(req, buf);
        }
        if (strlen(m->beeper_status) > 0) {
            snprintf(buf, sizeof(buf),
                "<tr><th>Beeper</th><td class='val'>%s</td></tr>",
                m->beeper_status);
            httpd_resp_sendstr_chunk(req, buf);
        }
    } else {
        httpd_resp_sendstr_chunk(req,
            "<tr><td colspan='2'>No valid UPS data available</td></tr>");
    }

    httpd_resp_sendstr_chunk(req, "</table></div>");

    /* Connection Info */
    snprintf(buf, sizeof(buf),
        "<div class='card'><h2>Connection</h2><table>"
        "<tr><th>WiFi</th><td class='val'>%s</td></tr>"
        "<tr><th>MQTT Broker</th><td class='val'>%s</td></tr>"
        "<tr><th>USB UPS</th><td class='val %s'>%s</td></tr>"
        "<tr><th>Publish Interval</th><td class='val'>%lu s</td></tr>"
        "</table></div>",
        current_config->wifi_ssid,
        current_config->mqtt_url,
        usb_ups_is_connected() ? "online" : "offline",
        usb_ups_is_connected() ? "Connected" : "Disconnected",
        (unsigned long)(current_config->publish_interval_ms / 1000));
    httpd_resp_sendstr_chunk(req, buf);

    /* Serial Logs */
    httpd_resp_sendstr_chunk(req,
        "<div class='card'><h2>Serial Logs</h2><pre id='logs'>");

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int start = (log_count < LOG_RING_SIZE) ? 0 : log_write_idx;
        int count = (log_count < LOG_RING_SIZE) ? log_count : LOG_RING_SIZE;

        for (int i = 0; i < count; i++) {
            int idx = (start + i) % LOG_RING_SIZE;
            char escaped[420];
            html_escape(escaped, log_ring[idx], sizeof(escaped));
            httpd_resp_sendstr_chunk(req, escaped);
            httpd_resp_sendstr_chunk(req, "\n");
        }
        xSemaphoreGive(log_mutex);
    }

    httpd_resp_sendstr_chunk(req,
        "</pre></div>"
        "<script>var l=document.getElementById('logs');l.scrollTop=l.scrollHeight;</script>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ═══════════════ GET /hid — Raw HID Capture (plain text) ═══════════════ */

/* Diagnostics for flag decoding. The parser assigns meanings to bit positions
 * that were never read off the device, so a wrong flag looks exactly like bad
 * data. This dumps what actually came over the wire: the report descriptor
 * (the device's own definition of every bit) and the last raw bytes of every
 * report ID seen. Plain text so it can be curl'd and diffed. */
static esp_err_t hid_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    char line[256];

    size_t desc_len = 0;
    const uint8_t *desc = hid_debug_get_descriptor(&desc_len);

    snprintf(line, sizeof(line), "# HID REPORT DESCRIPTOR (%u bytes)\n", (unsigned)desc_len);
    httpd_resp_sendstr_chunk(req, line);

    if (desc_len == 0) {
        httpd_resp_sendstr_chunk(req, "(not captured: UPS not enumerated, or the "
                                      "device STALLed GET_DESCRIPTOR)\n");
    } else {
        for (size_t i = 0; i < desc_len; i += 16) {
            int pos = snprintf(line, sizeof(line), "%04X  ", (unsigned)i);
            for (size_t j = 0; j < 16 && i + j < desc_len; j++) {
                pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", desc[i + j]);
            }
            snprintf(line + pos, sizeof(line) - pos, "\n");
            httpd_resp_sendstr_chunk(req, line);
        }
    }

    httpd_resp_sendstr_chunk(req,
        "\n# RAW REPORTS (last payload per report ID)\n"
        "# src=INT: pushed by the UPS on the interrupt endpoint\n"
        "# src=FEAT: returned by GET_REPORT(Feature)\n"
        "# chg=1 means the last payload differed from the one before it\n"
        "#  id  src   n  count      age_s  chg  bytes\n");

    static hid_dbg_report_t snap[HID_DBG_MAX_REPORTS];
    int n = hid_debug_get_reports(snap, HID_DBG_MAX_REPORTS);
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (int i = 0; i < n; i++) {
        const hid_dbg_report_t *r = &snap[i];
        int pos = snprintf(line, sizeof(line), "  %02X  %-4s  %2u  %6u  %9.1f    %d  ",
                           r->report_id,
                           r->src == HID_DBG_SRC_INTERRUPT ? "INT" : "FEAT",
                           r->len, (unsigned)r->count,
                           (now_ms - r->last_seen_ms) / 1000.0f,
                           r->changed ? 1 : 0);
        for (int j = 0; j < r->len; j++) {
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", r->data[j]);
        }
        snprintf(line + pos, sizeof(line) - pos, "\n");
        httpd_resp_sendstr_chunk(req, line);
    }

    if (n == 0) {
        httpd_resp_sendstr_chunk(req, "(no reports seen yet)\n");
    }

    /* What the parser made of it, so a wrong bit shows up next to its cause. */
    const ups_metrics_t *m = apc_hid_get_metrics();

    char ident[384];
    snprintf(ident, sizeof(ident),
             "\n# IDENTITY (USB string descriptors, not the report descriptor)\n"
             "manufacturer = %s\nmodel        = %s\nserial       = %s\n"
             "firmware     = %s\nchemistry    = %s\n",
             m->ups_manufacturer, m->ups_model, m->ups_serial,
             m->firmware_version, m->battery_type);
    httpd_resp_sendstr_chunk(req, ident);
    snprintf(line, sizeof(line),
             "\n# DECODED\nstatus=%s charge=%.0f%% runtime=%.0fs "
             "low_charge_thr=%.0f%% low_runtime_thr=%.0fs\n"
             "flags: OL=%d DISCHRG=%d CHRG=%d LB=%d OVER=%d RB=%d BOOST=%d TRIM=%d\n",
             m->status_string, m->battery_charge, m->battery_runtime,
             m->low_battery_charge_threshold, m->low_battery_runtime_threshold,
             m->status.online, m->status.discharging, m->status.charging,
             m->status.low_battery, m->status.overload, m->status.replace_battery,
             m->status.boost, m->status.trim);
    httpd_resp_sendstr_chunk(req, line);

    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ═══════════════ POST /save — Save Config & Reboot ═══════════════ */

static esp_err_t save_handler(httpd_req_t *req)
{
    char body[512];
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';

    app_config_t new_config = *current_config;
    char val[128];

    if (get_form_value(body, "wifi_ssid", val, sizeof(val)))
        strlcpy(new_config.wifi_ssid, val, sizeof(new_config.wifi_ssid));
    /* Password fields render empty (never disclosed), so an empty submission
       means "leave it alone". Only a non-empty value overwrites the stored one,
       and the explicit checkbox is the only way to erase one. */
    if (get_form_value(body, "wifi_pass_clear", val, sizeof(val)))
        new_config.wifi_pass[0] = '\0';
    else if (get_form_value(body, "wifi_pass", val, sizeof(val)) && val[0])
        strlcpy(new_config.wifi_pass, val, sizeof(new_config.wifi_pass));
    if (get_form_value(body, "mqtt_url", val, sizeof(val)))
        strlcpy(new_config.mqtt_url, val, sizeof(new_config.mqtt_url));
    if (get_form_value(body, "mqtt_user", val, sizeof(val)))
        strlcpy(new_config.mqtt_user, val, sizeof(new_config.mqtt_user));
    if (get_form_value(body, "mqtt_pass_clear", val, sizeof(val)))
        new_config.mqtt_pass[0] = '\0';
    else if (get_form_value(body, "mqtt_pass", val, sizeof(val)) && val[0])
        strlcpy(new_config.mqtt_pass, val, sizeof(new_config.mqtt_pass));
    if (get_form_value(body, "interval", val, sizeof(val))) {
        int secs = atoi(val);
        if (secs >= 5 && secs <= 300)
            new_config.publish_interval_ms = (uint32_t)secs * 1000;
    }
    if (get_form_value(body, "device_slug", val, sizeof(val)))
        sanitize_slug(new_config.device_slug, val, sizeof(new_config.device_slug));
    if (get_form_value(body, "device_name", val, sizeof(val)))
        sanitize_name(new_config.device_name, val, sizeof(new_config.device_name));

    esp_err_t err = config_save(&new_config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config saved to NVS, rebooting...");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>");
    httpd_resp_sendstr_chunk(req, PAGE_STYLE);
    httpd_resp_sendstr_chunk(req,
        "</style></head><body>"
        "<h1>Config Saved!</h1>"
        "<p style='text-align:center'>Rebooting in 2 seconds...</p>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;
}

/* ═══════════════ LED Probe ═══════════════ */

/* GET /led?gpio=48&ws=1&r=255&g=0&b=0   rebind and/or hold a colour
 * GET /led?auto=1                        resume the status pattern
 * GET /led                               report current binding
 *
 * Exists because these boards lose their serial console to USB host mode, so
 * finding which pin the LED actually sits on would otherwise cost a reflash
 * per guess. */
static esp_err_t led_probe_handler(httpd_req_t *req)
{
    char q[128] = {0};
    char v[16];
    bool acted = false;

    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        int gpio = -1;
        bool ws = true;

        if (httpd_query_key_value(q, "ws", v, sizeof(v)) == ESP_OK) ws = (atoi(v) != 0);

        if (httpd_query_key_value(q, "gpio", v, sizeof(v)) == ESP_OK) {
            gpio = atoi(v);
            led_status_reinit(gpio, ws);
            acted = true;
        }

        /* Binary-search an unknown indicator LED: /led?from=1&to=18&level=1 */
        if (httpd_query_key_value(q, "from", v, sizeof(v)) == ESP_OK) {
            int from = atoi(v), to = from, level = 1;
            if (httpd_query_key_value(q, "to", v, sizeof(v)) == ESP_OK)    to = atoi(v);
            if (httpd_query_key_value(q, "level", v, sizeof(v)) == ESP_OK) level = atoi(v);
            httpd_resp_set_type(req, "text/plain");
            return httpd_resp_sendstr(req, led_status_sweep(from, to, level));
        }

        if (httpd_query_key_value(q, "auto", v, sizeof(v)) == ESP_OK) {
            led_status_auto();
            acted = true;
        } else {
            int r = 0, g = 0, b = 0;
            bool have = false;
            if (httpd_query_key_value(q, "r", v, sizeof(v)) == ESP_OK) { r = atoi(v); have = true; }
            if (httpd_query_key_value(q, "g", v, sizeof(v)) == ESP_OK) { g = atoi(v); have = true; }
            if (httpd_query_key_value(q, "b", v, sizeof(v)) == ESP_OK) { b = atoi(v); have = true; }
            if (have) {
                led_status_test((uint8_t)r, (uint8_t)g, (uint8_t)b);
                acted = true;
            }
        }
    }

    char out[192];
    snprintf(out, sizeof(out), "%s%s\n", led_status_info(), acted ? "  [applied]" : "");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, out);
}

/* ═══════════════ Captive-Portal Redirect ═══════════════ */

/* Any unmatched GET while the portal is up bounces to the config page. iOS and
 * Android decide "this network needs sign-in" from exactly this 302. */
static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP_STR "/");
    return httpd_resp_send(req, NULL, 0);
}

/* ═══════════════ Server Start ═══════════════ */

static httpd_handle_t server = NULL;

esp_err_t http_server_start(app_config_t *config)
{
    current_config = config;

    /* Start log capture */
    log_mutex = xSemaphoreCreateMutex();
    if (log_mutex) {
        original_vprintf_fn = esp_log_set_vprintf(capture_vprintf);
        ESP_LOGI(TAG, "Log capture enabled (%d lines)", LOG_RING_SIZE);
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.stack_size = 8192;
    httpd_config.max_uri_handlers = 8;
    /* Generous socket timeouts, in seconds. A full OTA image is about 1MB and
       the upload competes with the USB host and MQTT tasks, so throughput can
       fall well below what a LAN transfer suggests. Anything tighter than this
       aborts large uploads part way through. */
    httpd_config.recv_wait_timeout = 300;
    httpd_config.send_wait_timeout = 300;
    /* One stalled socket must not be able to hold the only worker forever.
       Purging the least-recently-used connection keeps the UI reachable. */
    httpd_config.lru_purge_enable = true;
    /* Needed for the wildcard captive-portal catch-all below. */
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&server, &httpd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root_uri   = { .uri = "/",       .method = HTTP_GET,  .handler = root_handler     };
    const httpd_uri_t status_uri = { .uri = "/status",  .method = HTTP_GET,  .handler = status_handler   };
    const httpd_uri_t save_uri   = { .uri = "/save",    .method = HTTP_POST, .handler = save_handler     };
    const httpd_uri_t ota_uri    = { .uri = "/ota",     .method = HTTP_POST, .handler = ota_post_handler };
    const httpd_uri_t led_uri    = { .uri = "/led",     .method = HTTP_GET,  .handler = led_probe_handler };
    const httpd_uri_t hid_uri    = { .uri = "/hid",     .method = HTTP_GET,  .handler = hid_handler      };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &hid_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &led_uri);

    /* Captive-portal catch-all, registered LAST so the real routes win, and only
     * in portal mode — in STA mode a stray URL must 404, not bounce to 192.168.4.1.
     * Phones probe well-known URLs (captive.apple.com, /generate_204); the 302 is
     * what makes the sign-in sheet pop up. */
    if (wifi_portal_active()) {
        const httpd_uri_t catch_uri = { .uri = "/*", .method = HTTP_GET, .handler = redirect_handler };
        httpd_register_uri_handler(server, &catch_uri);
        ESP_LOGI(TAG, "Captive-portal catch-all registered (302 -> http://%s/)", PORTAL_IP_STR);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", httpd_config.server_port);
    return ESP_OK;
}
