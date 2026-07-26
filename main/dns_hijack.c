#include "dns_hijack.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "dns_hijack";

#define DNS_PORT      53
#define DNS_BUF_LEN   512
#define DNS_TTL       60

static TaskHandle_t s_dns_task = NULL;
static int          s_sock     = -1;
static uint32_t     s_ip       = 0;
static volatile bool s_running = false;

/* Walk the QNAME label sequence and return the offset just past it, or -1. */
static int skip_qname(const uint8_t *buf, int len, int pos)
{
    while (pos < len) {
        uint8_t l = buf[pos];
        if (l == 0) return pos + 1;
        if ((l & 0xC0) == 0xC0) return (pos + 2 <= len) ? pos + 2 : -1;  /* compressed */
        pos += l + 1;
    }
    return -1;
}

static void dns_hijack_task(void *arg)
{
    uint8_t buf[DNS_BUF_LEN];

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        s_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* 1s receive timeout so the loop yields and can observe s_running */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Bind the AP address specifically, not INADDR_ANY */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = s_ip,
    };

    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: errno %d", errno);
        close(s_sock);
        s_sock = -1;
        s_running = false;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "🕸️  DNS hijack listening on :%d", DNS_PORT);

    while (s_running) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);

        int len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (len < 0) continue;                        /* recv timeout — loop and yield */
        if (len < 12) continue;                       /* shorter than a DNS header */

        /* Answer everything with the portal address, whatever was asked. */
        buf[2]  = 0x81; buf[3]  = 0x80;               /* standard response, no error */
        buf[4]  = 0x00; buf[5]  = 0x01;               /* questions  = 1 */
        buf[6]  = 0x00; buf[7]  = 0x01;               /* answers    = 1 */
        buf[8]  = 0x00; buf[9]  = 0x00;               /* authority  = 0 */
        buf[10] = 0x00; buf[11] = 0x00;               /* additional = 0 */

        int pos = skip_qname(buf, len, 12);
        if (pos < 0) continue;
        pos += 4;                                     /* past QTYPE + QCLASS */
        if (pos + 16 > (int)sizeof(buf)) continue;

        buf[pos++] = 0xC0; buf[pos++] = 0x0C;         /* name -> offset 12 */
        buf[pos++] = 0x00; buf[pos++] = 0x01;         /* TYPE  A  */
        buf[pos++] = 0x00; buf[pos++] = 0x01;         /* CLASS IN */
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = DNS_TTL;      /* TTL */
        buf[pos++] = 0x00; buf[pos++] = 0x04;         /* RDLENGTH */
        memcpy(&buf[pos], &s_ip, 4);                  /* RDATA */
        pos += 4;

        sendto(s_sock, buf, pos, 0, (struct sockaddr *)&src, src_len);
    }

    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    ESP_LOGI(TAG, "DNS hijack stopped");
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_hijack_start(uint32_t resolve_ip)
{
    if (s_running) return ESP_OK;

    s_ip = resolve_ip;
    s_running = true;

    if (xTaskCreate(dns_hijack_task, "dns_hijack", 3072, NULL, 5, &s_dns_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dns_hijack_stop(void)
{
    if (!s_running) return;
    s_running = false;
    if (s_sock >= 0) {
        shutdown(s_sock, 0);   /* unblock recvfrom so the task can exit */
        close(s_sock);
        s_sock = -1;
    }
}
