/*
 * ═══════════════════════════════════════════════════════════════════════════
 * APC UPS USB HOST MANAGER - Comprehensive Architecture Overview
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 * This module handles USB communication with an APC Back-UPS via USB HID protocol.
 * The ESP32-S3 acts as a USB HOST (like your computer), and the UPS acts as a
 * USB DEVICE (like a keyboard or mouse).
 *
 * WHY TWO TYPES OF USB TRANSFERS?
 * ─────────────────────────────────────────────────────────────────────────
 * 1. INTERRUPT TRANSFERS (Automatic, pushed by UPS):
 *    - The UPS automatically sends status updates every ~200-2000ms
 *    - These contain: battery charge, runtime, status bits (online/charging)
 *    - Think of this like the UPS "tapping you on the shoulder" with updates
 *    - Example reports: 0x06 (status), 0x0C (charge+runtime), 0x16 (detailed status)
 *
 * 2. CONTROL TRANSFERS / GET_REPORT (On-demand, we ask the UPS):
 *    - We must actively REQUEST certain data by asking for specific "Feature Reports"
 *    - These contain: voltage, load percentage, transfer thresholds
 *    - Think of this like us "asking a question" and waiting for the answer
 *    - Example reports: 0x09 (battery voltage), 0x31 (input voltage), 0x50 (load %)
 *
 * WHY DOESN'T THE UPS SEND EVERYTHING VIA INTERRUPTS?
 * ─────────────────────────────────────────────────────────────────────────
 * - USB HID devices categorize data into "Input Reports" (pushed) and
 *   "Feature Reports" (polled on request)
 * - Status data that changes frequently → Input Reports (interrupt transfers)
 * - Configuration/slow-changing data → Feature Reports (control transfers)
 * - This is standard HID behavior, not specific to APC
 *
 * THE CALLBACK MYSTERY - WHY WAS IT SO HARD?
 * ─────────────────────────────────────────────────────────────────────────
 * In ESP-IDF, USB transfers complete asynchronously via CALLBACKS:
 * - When you submit a transfer, it returns immediately (non-blocking)
 * - Later, when data arrives, a callback function fires
 * - BUT: Callbacks only fire when you call usb_host_xxx_handle_events()
 *
 * TWO LEVELS OF EVENT HANDLING (This was the key breakthrough!):
 * 1. usb_host_lib_handle_events()    - Library level (device connections, control transfers)
 * 2. usb_host_client_handle_events() - Client level (transfer completion callbacks)
 *
 * FOR INTERRUPT TRANSFERS: Only client events needed
 * FOR CONTROL TRANSFERS: BOTH lib AND client events needed
 *
 * This is why GET_REPORT was timing out - we weren't processing library events!
 *
 * THREAD SAFETY:
 * ─────────────────────────────────────────────────────────────────────────
 * - transfer_mutex: Prevents interrupt and control transfers from running simultaneously
 * - transfer_done: Semaphore to signal when a transfer callback has fired
 *
 * DATA FLOW:
 * ─────────────────────────────────────────────────────────────────────────
 * USB Device → Interrupt Transfer → Raw HID Report (bytes) →
 * apc_hid_parser.c (decode) → ups_metrics_t struct →
 * main.c (MQTT publish) → Home Assistant
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include "usb_host_manager.h"
#include "apc_hid_parser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include <string.h>

static const char *TAG = "usb_host";

//══════════════════════════════════════════════════════════════════════════════
// USB DEVICE IDENTIFICATION
//══════════════════════════════════════════════════════════════════════════════
// APC UPS USB Vendor/Product IDs (identifies this specific UPS model)
// VID 0x051D = American Power Conversion
// PID 0x0002 = Back-UPS series (covers many models including XS 1000M)
#define APC_VID 0x051d
#define APC_PID 0x0002

//══════════════════════════════════════════════════════════════════════════════
// USB HOST STATE TRACKING
//══════════════════════════════════════════════════════════════════════════════
// These variables track the current state of the USB connection
static bool ups_connected = false;           // Is UPS physically connected?
static SemaphoreHandle_t usb_mutex = NULL;   // Mutex for USB library access
static usb_host_client_handle_t usb_client = NULL;  // Our USB client handle
static usb_device_handle_t ups_device = NULL;       // Handle to the UPS device

//══════════════════════════════════════════════════════════════════════════════
// HID (Human Interface Device) CONFIGURATION
//══════════════════════════════════════════════════════════════════════════════
// HID Interface: UPS uses interface 0 for all HID communication
#define HID_INTERFACE 0

// HID Interrupt Endpoint: 0x81 means IN endpoint 1 (device-to-host)
// This is where the UPS automatically sends status updates
#define HID_INTERRUPT_IN_EP 0x81

// USB Host client event handler
static void usb_host_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    ESP_LOGI(TAG, "DEBUG: Event callback triggered, event=%d", event_msg->event);

    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            ESP_LOGI(TAG, "🆕 New USB device detected (addr=%d)", event_msg->new_dev.address);

            // Open the device
            usb_device_handle_t dev_hdl;
            esp_err_t err = usb_host_device_open(usb_client, event_msg->new_dev.address, &dev_hdl);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open device: %s", esp_err_to_name(err));
                break;
            }

            // Get device descriptor
            const usb_device_desc_t *dev_desc;
            err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get device descriptor: %s", esp_err_to_name(err));
                usb_host_device_close(usb_client, dev_hdl);
                break;
            }

            ESP_LOGI(TAG, "DEBUG: Device VID:PID = %04X:%04X", dev_desc->idVendor, dev_desc->idProduct);

            // Check if this is our APC UPS
            if (dev_desc->idVendor == APC_VID && dev_desc->idProduct == APC_PID) {
                ESP_LOGI(TAG, "🔌 APC UPS found! VID:PID = %04X:%04X",
                         dev_desc->idVendor, dev_desc->idProduct);

                ups_device = dev_hdl;
                ups_connected = true;

                // Claim HID interface FIRST (before inspecting)
                err = usb_host_interface_claim(usb_client, ups_device, HID_INTERFACE, 0);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to claim interface: %s", esp_err_to_name(err));
                } else {
                    ESP_LOGI(TAG, "✅ HID interface claimed successfully");
                }

                // Get configuration descriptor to inspect endpoints (after claiming)
                const usb_config_desc_t *config_desc;
                err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "📋 Config: %d interfaces", config_desc->bNumInterfaces);

                    // Parse interfaces and endpoints (don't claim again!)
                    int offset = 0;
                    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, HID_INTERFACE, 0, &offset);
                    if (intf) {
                        ESP_LOGI(TAG, "  Interface %d: class=0x%02X, endpoints=%d",
                                 HID_INTERFACE, intf->bInterfaceClass, intf->bNumEndpoints);

                        // Log endpoints
                        int ep_offset = offset;
                        for (int e = 0; e < intf->bNumEndpoints; e++) {
                            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, e, config_desc->wTotalLength, &ep_offset);
                            if (ep) {
                                ESP_LOGI(TAG, "    Endpoint 0x%02X: type=%d, maxPacket=%d",
                                         ep->bEndpointAddress,
                                         ep->bmAttributes & 0x03,
                                         ep->wMaxPacketSize);
                            }
                        }
                    }
                }
            } else {
                ESP_LOGI(TAG, "⚠️ Not an APC UPS (VID:PID = %04X:%04X), expected %04X:%04X",
                         dev_desc->idVendor, dev_desc->idProduct, APC_VID, APC_PID);
                usb_host_device_close(usb_client, dev_hdl);
            }
            break;

        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            ESP_LOGW(TAG, "🚫 USB device removed");
            if (event_msg->dev_gone.dev_hdl == ups_device) {
                ups_connected = false;
                ups_device = NULL;
                ESP_LOGI(TAG, "❌ APC UPS disconnected");
            }
            break;

        default:
            ESP_LOGI(TAG, "DEBUG: Unknown event %d", event_msg->event);
            break;
    }
}

//══════════════════════════════════════════════════════════════════════════════
// THREAD SYNCHRONIZATION FOR USB TRANSFERS
//══════════════════════════════════════════════════════════════════════════════

// transfer_done: Binary semaphore to signal when a USB transfer callback fires
// - We submit a transfer (returns immediately)
// - We wait on this semaphore
// - When data arrives, callback fires and gives this semaphore
// - We wake up and process the data
static SemaphoreHandle_t transfer_done;

// transfer_mutex: Prevents simultaneous interrupt + control transfers
// WHY NEEDED: The USB hardware can only handle one transfer at a time per endpoint
// - Interrupt transfers use endpoint 0x81
// - Control transfers use endpoint 0x00
// - But they share internal USB resources, so we serialize them
static SemaphoreHandle_t transfer_mutex;

//══════════════════════════════════════════════════════════════════════════════
// USB TRANSFER COMPLETION CALLBACK
//══════════════════════════════════════════════════════════════════════════════
// This function is called by the USB driver when a transfer completes
// CRITICAL: This runs in interrupt context, so keep it FAST and minimal
// - Just signal the semaphore
// - Don't do heavy processing here
// - Let the main task wake up and handle the data
static void transfer_callback(usb_transfer_t *transfer)
{
    // Signal that transfer is complete by "giving" the semaphore
    // The waiting task will wake up when it tries to "take" this semaphore
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(transfer_done, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

//══════════════════════════════════════════════════════════════════════════════
// GET_REPORT: REQUEST FEATURE REPORTS FROM THE UPS
//══════════════════════════════════════════════════════════════════════════════
// This function actively ASKS the UPS for specific data using HID GET_REPORT
//
// USB HID GET_REPORT Protocol:
// ────────────────────────────────────────────────────────────────────────────
// 1. We send a control transfer with:
//    - bmRequestType: 0xA1 (Device-to-Host, Class request, Interface recipient)
//    - bRequest: 0x01 (GET_REPORT - standard HID request)
//    - wValue: (ReportType << 8) | ReportID
//      * ReportType = 3 (Feature Report) - polled data like voltage, load
//      * ReportID = specific report we want (0x09=battery voltage, 0x31=input voltage, etc.)
//    - wIndex: 0 (HID interface number)
//    - wLength: How many bytes we expect back
//
// 2. UPS responds with the requested report data
//
// 3. Our callback fires when data arrives
//
// THE CRITICAL FIX - WHY TWO EVENT HANDLERS:
// ────────────────────────────────────────────────────────────────────────────
// In the wait loop, we MUST call BOTH:
// - usb_host_lib_handle_events()    → Processes control transfer at hardware level
// - usb_host_client_handle_events() → Fires our callback when data arrives
//
// If we only call client events (like we did initially), control transfers
// never complete because the library-level processing doesn't happen!
//
// This took HOURS to debug because:
// - Interrupt transfers only need client events
// - Control transfers need BOTH lib and client events
// - The documentation doesn't clearly explain this difference
//
static esp_err_t get_hid_report(uint8_t report_id, uint8_t *buffer, size_t buffer_size, size_t *actual_length)
{
    if (ups_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire mutex: Only one USB transfer at a time
    // This prevents interrupt and control transfers from interfering
    if (xSemaphoreTake(transfer_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire transfer mutex for GET_REPORT");
        return ESP_ERR_TIMEOUT;
    }

    // Prepare USB transfer for control request
    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(buffer_size + 8, 0, &transfer);  // +8 for setup packet
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate control transfer: %s", esp_err_to_name(err));
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Setup control GET_REPORT request
    // Request Type: 0xA1 = Device-to-Host, Class, Interface
    // Request: 0x01 = GET_REPORT
    // Value: (ReportType << 8) | ReportID, where ReportType=3 for Feature Report
    // Index: Interface number (0)
    // Length: Expected report size
    // NOTE: Changed from Input Reports (type 1) to Feature Reports (type 3)
    // because voltage/load/frequency are synchronous polled values, not async events
    transfer->device_handle = ups_device;
    transfer->bEndpointAddress = 0x00;  // Control endpoint
    transfer->callback = transfer_callback;
    transfer->context = NULL;
    transfer->num_bytes = buffer_size + 8;
    transfer->timeout_ms = 1000;

    // Fill setup packet
    usb_setup_packet_t *setup = (usb_setup_packet_t *)transfer->data_buffer;
    setup->bmRequestType = 0xA1;  // Device-to-Host, Class, Interface
    setup->bRequest = 0x01;        // GET_REPORT
    setup->wValue = (3 << 8) | report_id;  // Feature Report (type 3), Report ID
    setup->wIndex = HID_INTERFACE;
    setup->wLength = buffer_size;

    // Create semaphore if not already created
    if (transfer_done == NULL) {
        transfer_done = xSemaphoreCreateBinary();
        if (transfer_done == NULL) {
            ESP_LOGE(TAG, "Failed to create transfer semaphore");
            usb_host_transfer_free(transfer);
            xSemaphoreGive(transfer_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Submit transfer
    err = usb_host_transfer_submit_control(usb_client, transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit GET_REPORT for 0x%02X: %s", report_id, esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    ESP_LOGD(TAG, "🔍 Requesting report ID 0x%02X...", report_id);

    // Wait for transfer completion
    // CRITICAL: Must process BOTH lib and client events for control transfers
    const int max_wait_ms = 2000;
    const int poll_interval_ms = 10;
    int waited_ms = 0;
    bool transfer_complete = false;

    // ═══════════════════════════════════════════════════════════════════════
    // THE CRITICAL WAIT LOOP - This is what makes control transfers work!
    // ═══════════════════════════════════════════════════════════════════════
    while (waited_ms < max_wait_ms && !transfer_complete) {
        // STEP 1: Process library-level events (hardware USB processing)
        // This is ESSENTIAL for control transfers to actually execute
        uint32_t event_flags;
        usb_host_lib_handle_events(pdMS_TO_TICKS(5), &event_flags);

        // STEP 2: Process client-level events (fires our callback)
        // This checks if our transfer_callback has been called
        usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(5));

        // STEP 3: Check if callback fired (semaphore was given)
        // Non-blocking check (timeout=0) so we keep looping
        if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
            transfer_complete = true;
            break;
        }
        waited_ms += poll_interval_ms;
    }
    // ═══════════════════════════════════════════════════════════════════════

    if (transfer_complete) {
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            // Data starts after 8-byte setup packet
            *actual_length = transfer->actual_num_bytes - 8;
            if (*actual_length > 0 && *actual_length <= buffer_size) {
                memcpy(buffer, transfer->data_buffer + 8, *actual_length);
                ESP_LOGD(TAG, "✅ GET_REPORT 0x%02X: %d bytes", report_id, *actual_length);
                err = ESP_OK;
            } else {
                err = ESP_ERR_INVALID_SIZE;
            }
        } else if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGD(TAG, "⚠️  Report 0x%02X not available (STALL)", report_id);
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            ESP_LOGD(TAG, "⚠️  GET_REPORT 0x%02X failed, status=%d", report_id, transfer->status);
            err = ESP_FAIL;
        }
        usb_host_transfer_free(transfer);
    } else {
        ESP_LOGW(TAG, "⚠️  GET_REPORT 0x%02X timeout after %dms, aborting", report_id, max_wait_ms);
        ESP_LOGW(TAG, "   Transfer status: %d (0=no_device, 1=completed, 2=error, 3=timed_out, 4=cancelled, 5=stall, 6=overflow, 7=skipped)",
                 transfer->status);

        // Cancel and free the transfer
        // Don't wait forever - the UPS doesn't support this report ID
        usb_host_transfer_free(transfer);
        err = ESP_ERR_NOT_SUPPORTED;
    }

    // Release mutex
    xSemaphoreGive(transfer_mutex);
    return err;
}

// Read HID report from interrupt endpoint
static esp_err_t read_hid_report(uint8_t *buffer, size_t buffer_size, size_t *actual_length)
{
    if (ups_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire mutex to prevent concurrent transfers
    if (xSemaphoreTake(transfer_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to acquire transfer mutex for interrupt read");
        return ESP_ERR_TIMEOUT;
    }

    // Create semaphore if not already created
    if (transfer_done == NULL) {
        transfer_done = xSemaphoreCreateBinary();
        if (transfer_done == NULL) {
            ESP_LOGE(TAG, "Failed to create transfer semaphore");
            xSemaphoreGive(transfer_mutex);
            return ESP_ERR_NO_MEM;
        }
    }

    // Prepare USB transfer
    usb_transfer_t *transfer;
    esp_err_t err = usb_host_transfer_alloc(buffer_size, 0, &transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate transfer: %s", esp_err_to_name(err));
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Setup interrupt IN transfer
    transfer->device_handle = ups_device;
    transfer->bEndpointAddress = HID_INTERRUPT_IN_EP;
    transfer->callback = transfer_callback;
    transfer->context = NULL;
    transfer->num_bytes = buffer_size;
    transfer->timeout_ms = 1000;

    // Submit transfer
    err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to submit transfer: %s", esp_err_to_name(err));
        usb_host_transfer_free(transfer);
        xSemaphoreGive(transfer_mutex);
        return err;
    }

    // Wait for transfer to complete while processing USB events
    // The callback can ONLY fire when usb_host_client_handle_events() is called
    // So we must poll events while waiting, not just block on semaphore
    ESP_LOGD(TAG, "⏳ Waiting for transfer completion (endpoint 0x%02X)...", HID_INTERRUPT_IN_EP);

    // Poll for up to 2000ms (transfer timeout is 1000ms + extra margin for slow callbacks)
    const int max_wait_ms = 2000;
    const int poll_interval_ms = 10;
    int waited_ms = 0;
    bool transfer_complete = false;

    while (waited_ms < max_wait_ms && !transfer_complete) {
        // Process USB client events - this is where callbacks fire!
        usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(poll_interval_ms));

        // Check if semaphore was signaled (non-blocking check)
        if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
            transfer_complete = true;
            break;
        }

        waited_ms += poll_interval_ms;
    }

    if (transfer_complete) {
        ESP_LOGD(TAG, "🔔 Transfer callback fired, status=%d (after %dms)", transfer->status, waited_ms);

        // Copy data and get actual length
        if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
            *actual_length = transfer->actual_num_bytes;
            memcpy(buffer, transfer->data_buffer, transfer->actual_num_bytes);

            if (*actual_length > 0) {
                ESP_LOGI(TAG, "✅ HID report received: %d bytes", *actual_length);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, (*actual_length < 16) ? *actual_length : 16, ESP_LOG_INFO);
            }
            err = ESP_OK;
        } else if (transfer->status == USB_TRANSFER_STATUS_TIMED_OUT) {
            ESP_LOGD(TAG, "⏱️  Transfer timed out (USB level) - device not sending data");
            err = ESP_ERR_TIMEOUT;
        } else if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            ESP_LOGW(TAG, "⚠️  Transfer stalled - endpoint may not be ready");
            err = ESP_FAIL;
        } else if (transfer->status == USB_TRANSFER_STATUS_ERROR) {
            ESP_LOGW(TAG, "❌ Transfer error");
            err = ESP_FAIL;
        } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
            ESP_LOGW(TAG, "❌ Device disconnected");
            err = ESP_FAIL;
        } else {
            ESP_LOGW(TAG, "❌ Transfer failed with unknown status: %d", transfer->status);
            err = ESP_FAIL;
        }

        // CRITICAL: Only free after callback has fired
        usb_host_transfer_free(transfer);
    } else {
        // Timeout waiting for callback - this should be very rare
        // Continue waiting until callback fires to avoid memory corruption
        ESP_LOGW(TAG, "⚠️  App-level timeout (%dms), continuing to wait for USB callback...", max_wait_ms);

        // Keep processing events until callback fires (with no time limit)
        while (!transfer_complete) {
            usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(poll_interval_ms));
            if (xSemaphoreTake(transfer_done, 0) == pdTRUE) {
                transfer_complete = true;
                ESP_LOGW(TAG, "✅ Late callback received, freeing transfer");
                usb_host_transfer_free(transfer);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(poll_interval_ms));
        }
        err = ESP_ERR_TIMEOUT;
    }

    // Release mutex
    xSemaphoreGive(transfer_mutex);
    return err;
}

esp_err_t usb_host_init(void)
{
    ESP_LOGI(TAG, "DEBUG: usb_host_init() called");
    ESP_LOGI(TAG, "🚀 Initializing USB Host for APC UPS");
    ESP_LOGW(TAG, "⚠️ Note: Many ESP32-S3 dev boards don't expose USB OTG pins");

    // Create mutex
    ESP_LOGI(TAG, "DEBUG: Creating USB mutex");
    usb_mutex = xSemaphoreCreateMutex();
    if (usb_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create USB mutex");
        return ESP_FAIL;
    }

    // Create transfer mutex to serialize transfers
    transfer_mutex = xSemaphoreCreateMutex();
    if (transfer_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Failed to create transfer mutex");
        return ESP_FAIL;
    }

    // Install USB Host library
    ESP_LOGI(TAG, "DEBUG: Installing USB Host library");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    esp_err_t ret = usb_host_install(&host_config);
    ESP_LOGI(TAG, "DEBUG: usb_host_install returned: 0x%x", ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to install USB host: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "💡 Your board may not support USB OTG on external pins");
        ESP_LOGW(TAG, "📝 Continuing with simulated data...");
        return ret;
    }

    // Register USB host client
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_host_client_event_cb,
            .callback_arg = NULL
        }
    };

    ret = usb_host_client_register(&client_config, &usb_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to register USB client: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "💡 USB OTG not available on this board");
        usb_host_uninstall();
        return ret;
    }

    ESP_LOGI(TAG, "✅ USB Host initialized successfully");
    ESP_LOGI(TAG, "🔍 Waiting for APC UPS (VID:PID = %04X:%04X)", APC_VID, APC_PID);

    return ESP_OK;
}

void usb_host_task(void *arg)
{
    ESP_LOGI(TAG, "📡 USB Host task started");
    ESP_LOGI(TAG, "DEBUG: Polling for USB events every 100ms");

    uint8_t report_buffer[64];
    size_t report_len;
    int error_count = 0;
    const int MAX_ERRORS = 10;
    int loop_count = 0;
    int poll_cycle = 0;

    // Report IDs to actively poll (verified from NUT explore output)
    // These are Feature Reports that MUST be polled, NOT sent via interrupt
    // Organized by category for clarity
    const uint8_t poll_reports[] = {
        // === CRITICAL REAL-TIME METRICS (poll every cycle) ===
        0x09,  // Battery voltage (UPS.PowerSummary.Voltage) - 16-bit, /100 for V
        0x31,  // Input voltage (UPS.Input.Voltage) - 16-bit
        0x50,  // Load percentage (UPS.PowerConverter.PercentLoad) - 8-bit

        // === BATTERY INFORMATION ===
        0x08,  // Battery nominal voltage (UPS.PowerSummary.ConfigVoltage) - 16-bit (12V)
        0x0E,  // Full charge capacity (100% - not used, just logged)
        0x0F,  // Battery charge warning threshold (50%)
        0x11,  // Battery charge low threshold (UPS.PowerSummary.RemainingCapacityLimit = 10%)
        0x24,  // Battery runtime low threshold (UPS.Battery.RemainingTimeLimit = 120s)
        0x17,  // Reboot timer (120s)
        0x03,  // Battery chemistry type (reports code 4 = NiMH)
        0x07,  // UPS manufacture date (days since reference = 21690)
        0x20,  // Battery manufacture date (days since reference = 21690)

        // === INPUT POWER CONFIGURATION ===
        0x30,  // Input nominal voltage (UPS.Input.ConfigVoltage) - 8-bit (120V)
        0x32,  // Low voltage transfer point (88V)
        0x33,  // High voltage transfer point (139V)
        0x34,  // Input sensitivity adjustment
        0x35,  // Input sensitivity (low/medium/high)
        0x36,  // Input frequency (50/60Hz)

        // === UPS CONFIGURATION ===
        0x52,  // Real power nominal (600W)
        0x15,  // Shutdown timer (-1 = not active)
        0x10,  // Beeper status (enabled/disabled/muted)
        0x18,  // Self-test result
    };
    const int num_poll_reports = sizeof(poll_reports) / sizeof(poll_reports[0]);

    while (1) {
        loop_count++;

        // Log every 50 loops (5 seconds) to show task is alive
        if (loop_count % 50 == 0) {
            ESP_LOGI(TAG, "DEBUG: USB task alive, loop %d, UPS connected: %d", loop_count, ups_connected);
        }

        // CRITICAL: Handle USB host LIBRARY events first (device connection/disconnection)
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(10), &event_flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "⚠️ USB lib event error: %s", esp_err_to_name(err));
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "⚠️ No USB clients registered");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "DEBUG: All devices freed");
        }

        // Handle USB host CLIENT events (our callback)
        err = usb_host_client_handle_events(usb_client, pdMS_TO_TICKS(10));

        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            error_count++;
            ESP_LOGW(TAG, "⚠️ USB client event error (%d/%d): %s",
                     error_count, MAX_ERRORS, esp_err_to_name(err));

            if (error_count >= MAX_ERRORS) {
                ESP_LOGE(TAG, "❌ USB Host failed too many times, disabling USB host");
                ESP_LOGE(TAG, "💡 Hint: This board may not support USB OTG on external pins");
                ESP_LOGE(TAG, "📝 Using simulated UPS data only");
                vTaskDelete(NULL);  // Kill this task
                return;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));  // Back off on errors
            continue;
        } else if (err == ESP_OK) {
            error_count = 0;  // Reset error count on success
            ESP_LOGI(TAG, "DEBUG: USB client event received (not timeout)");
        }

        // If UPS is connected, try to read HID reports
        if (ups_connected && ups_device != NULL) {
            // Passive: Read interrupt transfers (UPS sends automatically)
            err = read_hid_report(report_buffer, sizeof(report_buffer), &report_len);

            if (err == ESP_OK && report_len > 0) {
                // First byte is usually the report ID
                uint8_t report_id = report_buffer[0];

                ESP_LOGD(TAG, "📥 HID Report ID: 0x%02X, Length: %d", report_id, report_len);

                // Parse the report
                apc_hid_parse_report(report_id, report_buffer, report_len, NULL);
            }

            // RE-ENABLED: Using correct Feature Report IDs from NUT exploration
            // Poll on first loop and then every 20 loops (~40 seconds)
            if (loop_count == 1 || loop_count % 20 == 0) {
                ESP_LOGI(TAG, "🔄 Active polling cycle %d: Requesting %d reports...", poll_cycle++, num_poll_reports);

                for (int i = 0; i < num_poll_reports; i++) {
                    uint8_t report_id = poll_reports[i];
                    err = get_hid_report(report_id, report_buffer, sizeof(report_buffer), &report_len);

                    if (err == ESP_OK && report_len > 0) {
                        // Parse the polled report
                        apc_hid_parse_report(report_id, report_buffer, report_len, NULL);
                    }

                    // Small delay between polls to avoid overwhelming UPS
                    vTaskDelay(pdMS_TO_TICKS(20));
                }

                ESP_LOGI(TAG, "✅ Polling cycle %d complete", poll_cycle - 1);
            }
        }

        // Small delay
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool usb_ups_is_connected(void)
{
    return ups_connected;
}
