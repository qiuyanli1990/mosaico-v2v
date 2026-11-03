#pragma once

#include <stdint.h>
#include "tusb.h"

#define USB_UAC_TRACE_FIELDS(X) \
    X(out_packets) X(out_bytes) X(out_zero) X(out_errors) X(out_last_size) \
    X(out_last_us) X(out_submit_fail) X(fb_packets) X(fb_errors) \
    X(fb_submit_fail) X(fb_value) X(fb_size) X(speaker_alt) X(mic_alt) X(changes)

typedef struct {
#define USB_UAC_TRACE_FIELD(name) uint32_t name;
    USB_UAC_TRACE_FIELDS(USB_UAC_TRACE_FIELD)
#undef USB_UAC_TRACE_FIELD
} usb_uac_trace_t;

void usb_uac_trace_interface(uint8_t itf, uint8_t alt);
void usb_uac_trace_done(uint8_t ep, xfer_result_t result, uint32_t bytes);
bool usb_uac_trace_xfer(uint8_t rhport, uint8_t ep, uint8_t *data, uint16_t bytes);
void usb_uac_trace_get(usb_uac_trace_t *stats);
void usb_uac_trace_reset_interfaces(void);
