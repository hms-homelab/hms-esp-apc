#ifndef DNS_HIJACK_H
#define DNS_HIJACK_H

#include "esp_err.h"
#include <stdint.h>

/* Captive-portal DNS: answers EVERY A query with resolve_ip so any hostname
 * a client asks for lands on the config page. */
esp_err_t dns_hijack_start(uint32_t resolve_ip);   /* IPv4, network byte order */
void      dns_hijack_stop(void);

#endif // DNS_HIJACK_H
