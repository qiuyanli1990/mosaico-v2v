#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "device/usbd_pvt.h"
#include "usb_descriptors.h"
#include "usb_uac_trace.h"

static tusb_speed_t speed;
static uint8_t feedback[4];
static uint16_t feedback_size;
static unsigned feedback_submissions;
static uint8_t failed_ep;
static uint8_t *out_buffer;
static unsigned out_submissions;
static int64_t now_us;

tusb_speed_t tud_speed_get(void) { return speed; }
int64_t esp_timer_get_time(void) { return now_us; }
void usbd_int_set(bool enabled) { (void)enabled; }
void usbd_spin_lock(bool in_isr) { (void)in_isr; }
void usbd_spin_unlock(bool in_isr) { (void)in_isr; }
bool usbd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *ep)
{ (void)rhport; (void)ep; return true; }
void usbd_edpt_close(uint8_t rhport, uint8_t ep) { (void)rhport; (void)ep; }
void usbd_edpt_clear_stall(uint8_t rhport, uint8_t ep) { (void)rhport; (void)ep; }
void usbd_sof_enable(uint8_t rhport, sof_consumer_t consumer, bool enabled)
{ (void)rhport; (void)consumer; assert(!enabled); }
bool tud_control_status(uint8_t rhport, const tusb_control_request_t *request)
{ (void)rhport; (void)request; return true; }
bool tud_control_xfer(uint8_t rhport, const tusb_control_request_t *request, void *data, uint16_t bytes)
{ (void)rhport; (void)request; (void)data; (void)bytes; return true; }

bool usbd_edpt_xfer(uint8_t rhport, uint8_t ep, uint8_t *data, uint16_t bytes)
{
    (void)rhport;
    if (ep == 0x81) {
        assert(bytes <= sizeof(feedback));
        memset(feedback, 0, sizeof(feedback));
        /* Copy at submission, just as a controller can consume data immediately. */
        memcpy(feedback, data, bytes);
        feedback_size = bytes;
        ++feedback_submissions;
    } else if (ep == 0x01) {
        out_buffer = data;
        ++out_submissions;
    }
    return ep != failed_ep;
}

static void set_interface(uint8_t itf, uint8_t alt)
{
    tusb_control_request_t request = {
        .bmRequestType = 0x01, .bRequest = TUSB_REQ_SET_INTERFACE,
        .wIndex = itf, .wValue = alt,
    };
    assert(audiod_control_xfer_cb(0, CONTROL_STAGE_SETUP, &request));
}

static void mount_audio(void)
{
    audiod_init();
    const uint8_t *config = tud_descriptor_configuration_cb(0);
    unsigned total = config[2] | ((unsigned)config[3] << 8);
    for (unsigned offset = 0; offset < total; offset += config[offset]) {
        const tusb_desc_interface_t *itf = (const tusb_desc_interface_t *)(config + offset);
        if (itf->bDescriptorType == TUSB_DESC_INTERFACE &&
            itf->bInterfaceNumber == ITF_NUM_AUDIO_CONTROL) {
            assert(audiod_open(0, itf, total - offset));
            return;
        }
    }
    assert(false);
}

static void check_first_feedback(tusb_speed_t test_speed)
{
    speed = test_speed;
    mount_audio();
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        unsigned before = feedback_submissions;
        set_interface(ITF_NUM_AUDIO_STREAMING_SPK, 1);
        assert(feedback_submissions == before + 1);
        assert(feedback_size == (speed == TUSB_SPEED_HIGH ? 4 : 3));
        uint32_t value = feedback[0] | ((uint32_t)feedback[1] << 8) |
                         ((uint32_t)feedback[2] << 16) | ((uint32_t)feedback[3] << 24);
        assert(value == (speed == TUSB_SPEED_HIGH ? 0x00060000 : 0x000c0000));
        usb_uac_trace_t trace;
        usb_uac_trace_get(&trace);
        assert(trace.speaker_alt == 1 && trace.fb_value == value);
        /* Opening/closing the independent microphone must not resubmit feedback. */
        set_interface(ITF_NUM_AUDIO_STREAMING_MIC, 1);
        set_interface(ITF_NUM_AUDIO_STREAMING_MIC, 0);
        assert(feedback_submissions == before + 1);
        set_interface(ITF_NUM_AUDIO_STREAMING_SPK, 0);
        usb_uac_trace_get(&trace);
        assert(trace.speaker_alt == 0 && trace.mic_alt == 0);
    }
}

static void check_completion_counters(void)
{
    speed = TUSB_SPEED_HIGH;
    mount_audio();
    set_interface(ITF_NUM_AUDIO_STREAMING_SPK, 1);
    usb_uac_trace_t before, after;
    usb_uac_trace_get(&before);
    now_us = 123456;
    memset(out_buffer, 0x12, 24);
    assert(audiod_xfer_isr(0, 0x01, XFER_RESULT_SUCCESS, 24));
    unsigned before_zero = out_submissions;
    assert(audiod_xfer_isr(0, 0x01, XFER_RESULT_SUCCESS, 0));
    assert(out_submissions == before_zero + 1);
    failed_ep = 0x01;
    assert(audiod_xfer_isr(0, 0x01, XFER_RESULT_FAILED, 0));
    failed_ep = 0x81;
    assert(audiod_xfer_isr(0, 0x81, XFER_RESULT_FAILED, 4));
    failed_ep = 0;
    usb_uac_trace_get(&after);
    assert(after.out_packets - before.out_packets == 3);
    assert(after.out_bytes - before.out_bytes == 24);
    assert(after.out_zero - before.out_zero == 2);
    assert(after.out_errors - before.out_errors == 1);
    assert(after.out_submit_fail - before.out_submit_fail == 1);
    assert(after.out_last_us == now_us && after.out_last_size == 0);
    assert(after.fb_packets - before.fb_packets == 1);
    assert(after.fb_errors - before.fb_errors == 1);
    assert(after.fb_submit_fail - before.fb_submit_fail == 1);
    /* Diagnostic wrappers preserve the underlying transfer failure. */
    failed_ep = 0x81;
    assert(!usb_uac_trace_xfer(0, 0x81, feedback, 4));
    failed_ep = 0;
    usb_uac_trace_reset_interfaces();
    usb_uac_trace_get(&after);
    assert(after.speaker_alt == 0 && after.mic_alt == 0);
}

int main(void)
{
    check_first_feedback(TUSB_SPEED_HIGH);
    check_first_feedback(TUSB_SPEED_FULL);
    check_completion_counters();
    puts("PASS: real TinyUSB startup/reopen: first HS/FS feedback, independent MIC, zero-length OUT rearm, raw errors and failed submissions");
    return 0;
}
