#ifndef USB_HOST_MANAGER_H
#define USB_HOST_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t usb_host_init(void);
void usb_host_task(void *arg);
bool usb_ups_is_connected(void);

#endif // USB_HOST_MANAGER_H
