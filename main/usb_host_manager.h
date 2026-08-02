#ifndef USB_HOST_MANAGER_H
#define USB_HOST_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "hid_pdc.h"
#include "apc_hid_parser.h"

esp_err_t usb_host_init(void);
void usb_host_task(void *arg);
bool usb_ups_is_connected(void);

/* Metrics from the descriptor-driven decode, or NULL if no usable report
 * descriptor was parsed. Runs alongside apc_hid_parser while shadow mode is on;
 * /hid shows both so the two can be compared on a live board before MQTT is
 * switched over. */
const ups_metrics_t *usb_ups_generic_metrics(void);

/* The parsed field table, or NULL. */
const hid_pdc_map_t *usb_ups_pdc_map(void);

/* What is currently attached, whether or not it turned out to be a UPS. Lets
 * /hid identify unknown hardware without needing a NUT box. */
void usb_ups_attached_ids(uint16_t *vid, uint16_t *pid, bool *is_power_device);

#endif // USB_HOST_MANAGER_H
