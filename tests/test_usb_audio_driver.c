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
static uint8_t mic_packet[98];
static uint16_t mic_size;
static bool complete_mic_immediately;

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
    } else if (ep == 0x82) {
        assert(bytes <= sizeof(mic_packet));
        memcpy(mic_packet, data, bytes);
        mic_size = bytes;
        if (complete_mic_immediately) {
            complete_mic_immediately = false;
            usb_uac_trace_done(ep, XFER_RESULT_FAILED, 0);
        }
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
    before = after;
    usb_uac_trace_done(0x82, XFER_RESULT_SUCCESS, 96);
    usb_uac_trace_done(0x82, XFER_RESULT_FAILED, 0);
    failed_ep = 0x82;
    assert(!usb_uac_trace_xfer(0, 0x82, mic_packet, 0));
    failed_ep = 0;
    usb_uac_trace_get(&after);
    assert(after.in_packets - before.in_packets == 2);
    assert(after.in_bytes - before.in_bytes == 96);
    assert(after.in_zero - before.in_zero == 1);
    assert(after.in_errors - before.in_errors == 1);
    assert(after.in_submit_fail - before.in_submit_fail == 1);
    /* Diagnostic wrappers preserve the underlying transfer failure. */
    failed_ep = 0x81;
    assert(!usb_uac_trace_xfer(0, 0x81, feedback, 4));
    failed_ep = 0;
    usb_uac_trace_reset_interfaces();
    usb_uac_trace_get(&after);
    assert(after.speaker_alt == 0 && after.mic_alt == 0);
}

static void check_microphone_timing(void)
{
    usb_uac_trace_t before, after;
    usb_uac_trace_reset_interfaces();
    usb_uac_trace_get(&before);
    now_us = UINT32_MAX - 500;
    usb_uac_trace_done(0x82, XFER_RESULT_SUCCESS, 96);
    now_us += 1000;
    usb_uac_trace_done(0x82, XFER_RESULT_SUCCESS, 96);
    now_us += 3500;
    usb_uac_trace_done(0x82, XFER_RESULT_FAILED, 0);
    usb_uac_trace_get(&after);
    assert(after.in_gap_over_2ms == before.in_gap_over_2ms + 1);
    assert(after.in_gap_max_us == 3500);
    set_interface(ITF_NUM_AUDIO_STREAMING_MIC, 0);
    now_us += 5000000;
    set_interface(ITF_NUM_AUDIO_STREAMING_MIC, 1);
    usb_uac_trace_done(0x82, XFER_RESULT_SUCCESS, 96);
    usb_uac_trace_get(&after);
    assert(after.in_gap_over_2ms == before.in_gap_over_2ms + 1);
    assert(after.in_gap_max_us == 3500);
    usb_uac_trace_reset_interfaces();
    now_us = 0;
}

static void check_mic_failure_payloads(void)
{
    usb_uac_trace_t before, after;
    usb_uac_trace_reset_interfaces();
    usb_uac_trace_get(&before);
    assert(usb_uac_trace_xfer(0, 0x82, mic_packet, 96));
    usb_uac_trace_done(0x82, XFER_RESULT_FAILED, 0);
    assert(usb_uac_trace_xfer(0, 0x82, mic_packet, 0));
    usb_uac_trace_done(0x82, XFER_RESULT_FAILED, 0);
    usb_uac_trace_done(0x82, XFER_RESULT_FAILED, 0);
    complete_mic_immediately = true;
    assert(usb_uac_trace_xfer(0, 0x82, mic_packet, 94));
    failed_ep = 0x82;
    assert(!usb_uac_trace_xfer(0, 0x82, mic_packet, 96));
    failed_ep = 0;
    usb_uac_trace_mic_fifo(0, 94);
    usb_uac_trace_mic_fifo(92, 94);
    usb_uac_trace_mic_fifo(96, 94);
    usb_uac_trace_mic_feed(100);
    usb_uac_trace_mic_feed(50);
    usb_uac_trace_get(&after);
    assert(after.in_fail_pcm - before.in_fail_pcm == 2);
    assert(after.in_fail_pcm_bytes - before.in_fail_pcm_bytes == 190);
    assert(after.in_fail_zlp - before.in_fail_zlp == 1);
    assert(after.in_fail_unknown - before.in_fail_unknown == 1);
    assert(after.in_submit_fail - before.in_submit_fail == 1 && !after.in_pending);
    assert(after.in_fifo_empty - before.in_fifo_empty == 1);
    assert(after.in_fifo_short - before.in_fifo_short == 2 && after.in_feed_max_us == 100);
}

static void open_microphone(tusb_speed_t test_speed)
{
    speed = test_speed;
    mount_audio();
    uint32_t rate = 48000;
    tusb_control_request_t clock = {
        .bmRequestType = 0xa1, .bRequest = AUDIO_CS_REQ_CUR,
        .wIndex = (UAC2_ENTITY_CLOCK << 8) | ITF_NUM_AUDIO_CONTROL,
        .wValue = AUDIO_CS_CTRL_SAM_FREQ << 8, .wLength = sizeof(rate),
    };
    assert(tud_audio_buffer_and_schedule_control_xfer(0, &clock, &rate, sizeof(rate)));
    set_interface(ITF_NUM_AUDIO_STREAMING_MIC, 1);
}

static void check_mic_short_read(void)
{
    open_microphone(TUSB_SPEED_HIGH);
    uint16_t previous[48], current[47];
    for (unsigned i = 0; i < 48; ++i) previous[i] = 1000 + i;
    for (unsigned i = 0; i < 47; ++i) current[i] = 2000 + i;
    assert(tud_audio_n_write(0, previous, sizeof(previous)) == sizeof(previous));
    assert(audiod_xfer_isr(0, 0x82, XFER_RESULT_SUCCESS, 0));
    assert(mic_size == 94);
    tud_audio_n_clear_ep_in_ff(0);
    assert(tud_audio_n_write(0, previous, sizeof(previous)) == sizeof(previous));
    assert(audiod_xfer_isr(0, 0x82, XFER_RESULT_SUCCESS, mic_size));
    assert(mic_size == 96);
    assert(tud_audio_n_write(0, current, sizeof(current)) == sizeof(current));
    assert(audiod_xfer_isr(0, 0x82, XFER_RESULT_SUCCESS, mic_size));
    assert(mic_size == sizeof(current));
    assert(memcmp(mic_packet, current, sizeof(current)) == 0);
}

static void check_mic_sequence(tusb_speed_t test_speed)
{
    open_microphone(test_speed);
    uint16_t produced = 0, consumed = 0;
    unsigned packets = 0, empty = 0;
    for (unsigned tick = 0; tick < 2000; ++tick) {
        tu_fifo_t *fifo = tud_audio_n_get_ep_in_ff(0);
        if (!(tick >= 300 && tick < 325) && tu_fifo_remaining(fifo) >= 960) {
            uint16_t chunk[480];
            for (unsigned i = 0; i < 480; ++i) chunk[i] = produced++;
            assert(tud_audio_n_write(0, chunk, sizeof(chunk)) == sizeof(chunk));
        }
        assert(audiod_xfer_isr(0, 0x82, XFER_RESULT_SUCCESS, mic_size));
        assert(mic_size % sizeof(uint16_t) == 0);
        for (unsigned i = 0; i < mic_size; i += sizeof(uint16_t)) {
            uint16_t sample;
            memcpy(&sample, mic_packet + i, sizeof(sample));
            assert(sample == consumed++);
        }
        if (mic_size) ++packets;
        else ++empty;
    }
    assert(packets > 1900 && empty > 0);
    assert((uint16_t)(produced - consumed) < 540);
}

int main(void)
{
    check_mic_short_read();
    check_mic_sequence(TUSB_SPEED_HIGH);
    check_mic_sequence(TUSB_SPEED_FULL);
    check_first_feedback(TUSB_SPEED_HIGH);
    check_first_feedback(TUSB_SPEED_FULL);
    check_completion_counters();
    check_microphone_timing();
    check_mic_failure_payloads();
    puts("PASS: real TinyUSB HS/FS mic PCM sequence/short reads, feedback, interface reopen, IN timing and IN/OUT errors");
    return 0;
}
