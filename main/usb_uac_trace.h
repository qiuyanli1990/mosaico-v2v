#pragma once

#include <stdint.h>
#include "tusb.h"

#define USB_UAC_TRACE_FIELDS(X) \
    X(out_packets) X(out_bytes) X(out_zero) X(out_errors) X(out_last_size) \
    X(out_last_us) X(out_submit_fail) X(fb_packets) X(fb_errors) \
    X(fb_submit_fail) X(fb_value) X(fb_size) X(speaker_alt) X(mic_alt) X(changes) \
    X(in_packets) X(in_bytes) X(in_zero) X(in_errors) X(in_submit_fail) \
    X(in_last_us) X(in_gap_max_us) X(in_gap_over_2ms) \
    X(in_tx_pcm) X(in_tx_zlp) X(in_pending) X(in_fail_pcm) X(in_fail_zlp) \
    X(in_fail_unknown) X(in_fail_pcm_bytes) X(in_fifo_empty) X(in_fifo_short) \
    X(in_feed_max_us)

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
void usb_uac_trace_mic_fifo(uint16_t queued, uint16_t minimum);
void usb_uac_trace_mic_feed(uint32_t elapsed_us);
