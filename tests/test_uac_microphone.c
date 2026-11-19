#define main usb_driver_checks
#include "test_usb_audio_driver.c"
#undef main
#include "usb_audio.c"

unsigned critical_depth, critical_entries, mutex_depth, mutex_creations, signal_creations;
unsigned isr_depth, isr_entries, isr_gives;
static unsigned uac_depth;
#define UAC_ENTER_CRITICAL() (++uac_depth)
#define UAC_EXIT_CRITICAL() (--uac_depth)
typedef struct {
    bool mic_active;
    uint32_t mic_generation;
    int mic_data_size;
    uint8_t buffer[1078];
    uint8_t *mic_buf_read;
} uac_device_t;
static uac_device_t device, *s_uac_device = &device;

esp_err_t uac_device_init(uac_device_config_t *config) { (void)config; return ESP_OK; }
void vTaskDelay(unsigned ticks) { (void)ticks; assert(false); }
void test_usb_wait_for_signal(void) { assert(!"Unexpected producer blocking in pipeline test"); }
#include "uac_mic_functions.inc"

static uint16_t next_input, expected_output;
static int16_t input_pcm[9600];

static void fill_application(void)
{
    size_t count = MIC_OUTPUT_BUFFER_SAMPLES - microphone_output.count;
    for (size_t i = 0; i < count; ++i) input_pcm[i] = (int16_t)next_input++;
    usb_audio_write_microphone(input_pcm, count);
}

static void fixture(tusb_speed_t test_speed)
{
    memset(&device, 0, sizeof(device));
    device.mic_buf_read = device.buffer;
    next_input = expected_output = 0;
    now_us = 1;
    tud_mount_cb();
    usb_audio_set_forwarding(true, true);
    usb_audio_set_microphone_active(true);
    fill_application();
    open_microphone(test_speed);
    device.mic_active = true;
}

static void complete(bool failed)
{
    ++isr_depth;
    assert(audiod_xfer_isr(0, 0x82, failed ? XFER_RESULT_FAILED : XFER_RESULT_SUCCESS,
                          failed ? 0 : mic_size));
    --isr_depth;
    assert(!critical_depth && !uac_depth);
}

static void check_pipeline(tusb_speed_t test_speed, unsigned pause_ms, bool fail_pcm)
{
    fixture(test_speed);
    unsigned empty = 0, gaps = 0;
    bool failed = false;
    usb_uac_trace_t before, after;
    usb_uac_trace_get(&before);
    for (unsigned tick = 0; tick < 1200; ++tick) {
        now_us = (int64_t)tick * 1000 + 1;
        if (tick == 300 && pause_ms) lock_input();
        if (tick == 300 + pause_ms && pause_ms) unlock_input();
        /* No task can move PCM while this mutex is held. USB ISR must continue. */
        if (!mutex_depth && tick && tick % 20 == 0) fill_application();
        bool inject = fail_pcm && !failed && tick >= 500 && mic_size == 96;
        if (inject) failed = true;
        if (!mic_size && tick > 10) ++empty;
        if (!inject) {
            for (unsigned at = 0; at < mic_size; at += 2) {
                uint16_t sample;
                memcpy(&sample, mic_packet + at, sizeof(sample));
                gaps += (uint16_t)(sample - expected_output);
                expected_output = sample + 1;
            }
        }
        complete(inject);
        if (tick > 10) assert(mic_size == 96);
    }
    usb_uac_trace_get(&after);
    assert(!empty && gaps == (fail_pcm ? 48u : 0u));
    assert(state.mic_underruns == 0 && microphone_output.dropped == 0);
    assert(after.in_fail_pcm - before.in_fail_pcm == (fail_pcm ? 1u : 0u));
    assert(after.in_fail_pcm_bytes - before.in_fail_pcm_bytes == (fail_pcm ? 96u : 0u));
    assert(after.in_fail_zlp == before.in_fail_zlp);
    printf("PASS: real %s USB mic/app/FIFO; task pause=%u ms, ZLPs=%u, gaps=%u, failed PCM=%d\n",
           test_speed == TUSB_SPEED_HIGH ? "HS" : "FS", pause_ms, empty, gaps, fail_pcm);
}

static void check_starvation_and_route(void)
{
    fixture(TUSB_SPEED_HIGH);
    for (unsigned tick = 0; tick < 350; ++tick) {
        now_us = (int64_t)tick * 1000 + 1;
        complete(false);
        if (tick > 10) assert(mic_size == 96);
    }
    assert(state.mic_underruns > 0 && !microphone_output.count);
    for (unsigned i = 0; i < mic_size; ++i) assert(!mic_packet[i]);
    usb_audio_set_forwarding(true, false);
    set_interface(ITF_NUM_AUDIO_STREAMING_MIC, 0);
    stop_microphone();
    assert(!device.mic_active && !microphone_route_open);
    usb_audio_write_microphone(input_pcm, 480);
    assert(!microphone_output.count);
    usb_audio_set_forwarding(true, true);
    usb_audio_set_microphone_active(true);
    next_input = expected_output = 0x4000;
    fill_application();
    set_interface(ITF_NUM_AUDIO_STREAMING_MIC, 1);
    device.mic_active = true;
    for (unsigned tick = 0; tick < 40; ++tick) {
        for (unsigned at = 0; at < mic_size; at += 2) {
            uint16_t sample;
            memcpy(&sample, mic_packet + at, sizeof(sample));
            assert(sample == expected_output++);
        }
        now_us += 1000;
        complete(false);
    }
    puts("PASS: genuine source starvation pads PCM silence; close/reopen clears old USB PCM");
}

int main(void)
{
    const unsigned pauses[] = {0, 12, 25, 49};
    for (unsigned i = 0; i < sizeof(pauses) / sizeof(pauses[0]); ++i) {
        check_pipeline(TUSB_SPEED_HIGH, pauses[i], false);
        check_pipeline(TUSB_SPEED_FULL, pauses[i], false);
    }
    check_pipeline(TUSB_SPEED_HIGH, 0, true);
    check_pipeline(TUSB_SPEED_FULL, 0, true);
    check_starvation_and_route();
    assert(!critical_depth && !mutex_depth && !isr_depth && isr_gives);
}
