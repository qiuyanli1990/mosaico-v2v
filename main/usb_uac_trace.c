#include <stdatomic.h>
#include "device/usbd_pvt.h"
#include "esp_timer.h"
#include "usb_descriptors.h"
#include "usb_uac_trace.h"

/* These hooks run at USB interrupt rate. Never log or take a task lock here. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "USB trace counters must be lock-free");
static struct {
#define USB_UAC_TRACE_FIELD(name) atomic_uint name;
    USB_UAC_TRACE_FIELDS(USB_UAC_TRACE_FIELD)
#undef USB_UAC_TRACE_FIELD
} state;

void usb_uac_trace_interface(uint8_t itf, uint8_t alt)
{
    if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
        atomic_store_explicit(&state.speaker_alt, alt, memory_order_relaxed);
    } else if (itf == ITF_NUM_AUDIO_STREAMING_MIC) {
        atomic_store_explicit(&state.mic_alt, alt, memory_order_relaxed);
    } else {
        return;
    }
    atomic_fetch_add_explicit(&state.changes, 1, memory_order_relaxed);
}

void usb_uac_trace_reset_interfaces(void)
{
    atomic_store_explicit(&state.speaker_alt, 0, memory_order_relaxed);
    atomic_store_explicit(&state.mic_alt, 0, memory_order_relaxed);
}

void usb_uac_trace_done(uint8_t ep, xfer_result_t result, uint32_t bytes)
{
    if (ep == 0x01) {
        atomic_fetch_add_explicit(&state.out_packets, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&state.out_bytes, bytes, memory_order_relaxed);
        atomic_store_explicit(&state.out_last_size, bytes, memory_order_relaxed);
        atomic_store_explicit(&state.out_last_us, (uint32_t)esp_timer_get_time(), memory_order_relaxed);
        if (!bytes) atomic_fetch_add_explicit(&state.out_zero, 1, memory_order_relaxed);
        if (result != XFER_RESULT_SUCCESS) {
            atomic_fetch_add_explicit(&state.out_errors, 1, memory_order_relaxed);
        }
    } else if (ep == 0x81) {
        atomic_fetch_add_explicit(&state.fb_packets, 1, memory_order_relaxed);
        if (result != XFER_RESULT_SUCCESS) {
            atomic_fetch_add_explicit(&state.fb_errors, 1, memory_order_relaxed);
        }
    }
}

bool usb_uac_trace_xfer(uint8_t rhport, uint8_t ep, uint8_t *data, uint16_t bytes)
{
    if (ep == 0x81) {
        uint32_t value = 0;
        for (unsigned i = 0; data && i < bytes && i < 4; ++i) {
            value |= (uint32_t)data[i] << (i * 8);
        }
        atomic_store_explicit(&state.fb_value, value, memory_order_relaxed);
        atomic_store_explicit(&state.fb_size, bytes, memory_order_relaxed);
    }
    bool ok = usbd_edpt_xfer(rhport, ep, data, bytes);
    if (!ok && ep == 0x01) {
        atomic_fetch_add_explicit(&state.out_submit_fail, 1, memory_order_relaxed);
    } else if (!ok && ep == 0x81) {
        atomic_fetch_add_explicit(&state.fb_submit_fail, 1, memory_order_relaxed);
    }
    return ok;
}

void usb_uac_trace_get(usb_uac_trace_t *stats)
{
    /* A live snapshot: each counter is atomic, the collection is approximate. */
#define USB_UAC_TRACE_FIELD(name) stats->name = atomic_load_explicit(&state.name, memory_order_relaxed);
    USB_UAC_TRACE_FIELDS(USB_UAC_TRACE_FIELD)
#undef USB_UAC_TRACE_FIELD
}
