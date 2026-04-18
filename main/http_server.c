#include "http_server.h"
#include "apc_hid_parser.h"
#include "usb_host_manager.h"
#include "wifi_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
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

static int capture_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);

    if (log_mutex && xSemaphoreTake(log_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        vsnprintf(log_ring[log_write_idx], LOG_LINE_LEN, fmt, copy);
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

    /* Override from NVS if previously saved */
    nvs_handle_t nvs;
    if (nvs_open("config", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(config->wifi_ssid);  nvs_get_str(nvs, "wifi_ssid", config->wifi_ssid, &len);
        len = sizeof(config->wifi_pass);  nvs_get_str(nvs, "wifi_pass", config->wifi_pass, &len);
        len = sizeof(config->mqtt_url);   nvs_get_str(nvs, "mqtt_url",  config->mqtt_url,  &len);
        len = sizeof(config->mqtt_user);  nvs_get_str(nvs, "mqtt_user", config->mqtt_user, &len);
        len = sizeof(config->mqtt_pass);  nvs_get_str(nvs, "mqtt_pass", config->mqtt_pass, &len);
        nvs_get_u32(nvs, "pub_interval", &config->publish_interval_ms);
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
    "<div class='nav'><a href='/'>Config</a> | <a href='/status'>Status &amp; Logs</a></div>";

/* ═══════════════ GET / — Config Page ═══════════════ */

static void send_page_header(httpd_req_t *req, const char *title, bool auto_refresh)
{
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
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
    httpd_resp_set_type(req, "text/html");
    char buf[512];

    send_page_header(req, "APC UPS Bridge", false);

    char ssid_esc[128], url_esc[256], user_esc[128];
    html_escape(ssid_esc, current_config->wifi_ssid, sizeof(ssid_esc));
    html_escape(url_esc,  current_config->mqtt_url,  sizeof(url_esc));
    html_escape(user_esc, current_config->mqtt_user,  sizeof(user_esc));

    httpd_resp_sendstr_chunk(req, "<form method='POST' action='/save'>");

    /* WiFi */
    snprintf(buf, sizeof(buf),
        "<div class='card'><h2>WiFi</h2>"
        "<label>SSID</label><input name='wifi_ssid' value='%s'>"
        "<label>Password</label><input name='wifi_pass' type='password' value='%s'>"
        "</div>",
        ssid_esc, current_config->wifi_pass);
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
        "<label>Password</label><input name='mqtt_pass' type='password' value='%s'>",
        current_config->mqtt_pass);
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
        "<button type='submit'>Save &amp; Reboot</button></form></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ═══════════════ GET /status — Metrics + Logs ═══════════════ */

static esp_err_t status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
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
    if (get_form_value(body, "wifi_pass", val, sizeof(val)))
        strlcpy(new_config.wifi_pass, val, sizeof(new_config.wifi_pass));
    if (get_form_value(body, "mqtt_url", val, sizeof(val)))
        strlcpy(new_config.mqtt_url, val, sizeof(new_config.mqtt_url));
    if (get_form_value(body, "mqtt_user", val, sizeof(val)))
        strlcpy(new_config.mqtt_user, val, sizeof(new_config.mqtt_user));
    if (get_form_value(body, "mqtt_pass", val, sizeof(val)))
        strlcpy(new_config.mqtt_pass, val, sizeof(new_config.mqtt_pass));
    if (get_form_value(body, "interval", val, sizeof(val))) {
        int secs = atoi(val);
        if (secs >= 5 && secs <= 300)
            new_config.publish_interval_ms = (uint32_t)secs * 1000;
    }

    esp_err_t err = config_save(&new_config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config saved to NVS, rebooting...");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!DOCTYPE html><html><head>"
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
    httpd_config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&server, &httpd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root_uri   = { .uri = "/",       .method = HTTP_GET,  .handler = root_handler   };
    const httpd_uri_t status_uri = { .uri = "/status",  .method = HTTP_GET,  .handler = status_handler };
    const httpd_uri_t save_uri   = { .uri = "/save",    .method = HTTP_POST, .handler = save_handler   };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", httpd_config.server_port);
    return ESP_OK;
}
