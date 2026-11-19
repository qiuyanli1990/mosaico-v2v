#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_audio.c"

static int64_t now_us;
static void (*wait_action)(void);
static unsigned wait_count;
static int16_t sequence[12000], recorded[12000];
static size_t recorded_count;
unsigned critical_depth, critical_entries, mutex_depth, mutex_creations, signal_creations;
unsigned isr_depth, isr_entries, isr_gives;

int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ticks)
{
    (void)ticks;
    assert(!"USB microphone writer must block on consumption, not poll");
}
void test_usb_wait_for_signal(void)
{
    assert(!critical_depth && !mutex_depth);
    assert(wait_action && ++wait_count < 100);
    now_us += 10000;
    wait_action();
}
esp_err_t uac_device_init(uac_device_config_t *config) { (void)config; return ESP_OK; }
tusb_speed_t tud_speed_get(void) { return TUSB_SPEED_HIGH; }
void usb_uac_trace_get(usb_uac_trace_t *trace) { memset(trace, 0, sizeof(*trace)); }
void usb_uac_trace_reset_interfaces(void) {}

static void fixture(void)
{
    memset(&state, 0, sizeof(state));
    microphone_output_enabled = false;
    now_us = 1000;
    wait_count = recorded_count = 0;
    wait_action = NULL;
    tud_mount_cb();
    usb_audio_set_microphone_active(true);
    usb_audio_set_forwarding(true, true);
    for (unsigned i = 0; i < 12000; ++i) sequence[i] = i + 1;
}

static void read_chunk(int16_t *data)
{
    ++isr_depth;
    usb_audio_read_microphone_isr(data, 480);
    --isr_depth;
}

static void check_task_mutex_does_not_block_isr(void)
{
    fixture();
    usb_audio_write_microphone(sequence, MIC_PREFILL_SAMPLES);
    int16_t output[480];
    lock_input();
    read_chunk(output);
    assert(mutex_depth == 1 && !critical_depth);
    unlock_input();
    assert(memcmp(output, sequence, sizeof(output)) == 0 && isr_gives);
}

static void record_chunk(void)
{
    assert(recorded_count + 480 <= 12000);
    read_chunk(recorded + recorded_count);
    recorded_count += 480;
}

static void check_prefill_and_tail(void)
{
    fixture();
    int16_t output[480], zero[480] = {0};
    usb_audio_write_microphone(sequence, 480);
    read_chunk(output);
    assert(memcmp(output, zero, sizeof(output)) == 0 && microphone_output.count == 480);
    usb_audio_write_microphone(sequence + 480, MIC_PREFILL_SAMPLES - 480);
    for (unsigned i = 0; i < MIC_PREFILL_SAMPLES / 480; ++i) {
        read_chunk(output);
        assert(memcmp(output, sequence + i * 480, sizeof(output)) == 0);
    }
    read_chunk(output);
    assert(state.mic_underruns == 1 && !microphone_playing);
    usb_audio_write_microphone(sequence, 100);
    read_chunk(output);
    assert(memcmp(output, zero, sizeof(output)) == 0 && microphone_output.count == 100);
    now_us += MIC_TAIL_WAIT_US;
    read_chunk(output);
    assert(memcmp(output, sequence, 100 * sizeof(int16_t)) == 0);
    assert(memcmp(output + 100, zero, 380 * sizeof(int16_t)) == 0);
    assert(microphone_output.count == 0 && state.mic_underruns == 2);
}

static void check_burst_backpressure(void)
{
    fixture();
    wait_action = record_chunk;
    size_t total = MIC_OUTPUT_BUFFER_SAMPLES + 960;
    usb_audio_write_microphone(sequence, total);
    while (recorded_count < total) record_chunk();
    assert(memcmp(recorded, sequence, total * sizeof(int16_t)) == 0);
    assert(wait_count == 2 && state.mic_write_waits == 2 && microphone_output.dropped == 0);
    assert(state.mic_written_samples == total && state.mic_underruns == 0);
}

static void close_and_reopen(void)
{
    usb_audio_set_microphone_active(false);
    usb_audio_set_microphone_active(true);
}
static void stop_forwarding(void) { usb_audio_set_forwarding(true, false); }
static void disconnect_usb(void) { tud_umount_cb(); }
static void suspend_usb(void) { tud_suspend_cb(false); }

static void check_cancelled_writer(void (*cancel)(void))
{
    fixture();
    usb_audio_write_microphone(sequence, MIC_OUTPUT_BUFFER_SAMPLES);
    wait_action = cancel;
    usb_audio_write_microphone(sequence, 480);
    assert(wait_count == 1 && microphone_output.count == 0);
    assert(state.mic_written_samples == MIC_OUTPUT_BUFFER_SAMPLES);
}

static void signal_without_consuming(void)
{
    xSemaphoreGive(microphone_space);
    wait_action = record_chunk;
}

static void check_spurious_space_signal(void)
{
    fixture();
    usb_audio_write_microphone(sequence, MIC_OUTPUT_BUFFER_SAMPLES);
    wait_action = signal_without_consuming;
    usb_audio_write_microphone(sequence + MIC_OUTPUT_BUFFER_SAMPLES, 480);
    while (recorded_count < MIC_OUTPUT_BUFFER_SAMPLES + 480) record_chunk();
    assert(wait_count == 2 && state.mic_write_waits == 2);
    assert(microphone_output.dropped == 0);
    assert(memcmp(recorded, sequence, (MIC_OUTPUT_BUFFER_SAMPLES + 480) * sizeof(*sequence)) == 0);
}

static void check_speaker_path(void)
{
    fixture();
    int16_t stereo[] = {100, 300, -100, -300}, mono[2];
    assert(receive_audio((uint8_t *)stereo, sizeof(stereo), NULL) == ESP_OK);
    assert(usb_audio_read(mono, 2) == 2 && mono[0] == 200 && mono[1] == -200);
}

static void check_suspend_resume(void)
{
    fixture();
    usb_audio_write_microphone(sequence, 480);
    tud_suspend_cb(false);
    assert(!state.mounted && !state.mic_active && microphone_output.count == 0);
    usb_audio_write_microphone(sequence, 480);
    assert(microphone_output.count == 0);
    tud_resume_cb();
    assert(state.mounted && state.mic_active);
    usb_audio_write_microphone(sequence, MIC_PREFILL_SAMPLES);
    int16_t output[480];
    read_chunk(output);
    assert(memcmp(output, sequence, sizeof(output)) == 0);
    usb_audio_set_microphone_active(false);
    tud_suspend_cb(false);
    tud_resume_cb();
    assert(!state.mic_active);
    usb_audio_set_microphone_active(true);
    tud_suspend_cb(false);
    tud_umount_cb();
    tud_mount_cb();
    assert(!state.mic_active && !microphone_resume_active);
}

static void check_frame_trace(void)
{
    fixture();
    uint8_t first[] = {1, 2, 3, 4}, second[] = {5, 6, 7, 8}, silence[4] = {0};
    usb_audio_trace_microphone_frame(100, first, sizeof(first));
    usb_audio_trace_microphone_frame(120, second, sizeof(second));
    usb_audio_trace_microphone_frame(100, first, sizeof(first));
    assert(state.mic_render_frames == 3 && state.mic_pts_back == 1 && state.mic_repeat_hints == 1);
    /* Repeated words at new timestamps and silent frames are not replay hints. */
    usb_audio_trace_microphone_frame(140, first, sizeof(first));
    usb_audio_trace_microphone_frame(140, second, sizeof(second));
    usb_audio_trace_microphone_frame(160, silence, sizeof(silence));
    usb_audio_trace_microphone_frame(160, silence, sizeof(silence));
    assert(state.mic_pts_same == 2 && state.mic_repeat_hints == 1);
    for (unsigned i = 0; i < 70; ++i) {
        usb_audio_trace_microphone_frame(180 + i * 20, second, sizeof(second));
    }
    usb_audio_trace_microphone_frame(100, first, sizeof(first));
    assert(state.mic_repeat_hints == 1); /* Old history expires. */
    usb_audio_clear();
    usb_audio_trace_microphone_frame(UINT32_MAX - 10, first, sizeof(first));
    uint32_t backwards = state.mic_pts_back;
    usb_audio_trace_microphone_frame(9, second, sizeof(second));
    assert(state.mic_pts_back == backwards); /* Timestamp rollover. */
    usb_audio_set_microphone_active(false);
    usb_audio_set_microphone_active(true);
    usb_audio_trace_microphone_frame(9, second, sizeof(second));
    assert(state.mic_repeat_hints == 1); /* No history across routes. */
    assert(microphone_output.count == 0 && state.mic_written_samples == 0);
}

static void check_callback_timing(void)
{
    fixture();
    int16_t output[480];
    read_chunk(output);
    now_us += 10000;
    read_chunk(output);
    assert(state.mic_callback_late == 0 && state.mic_callback_max_gap_us == 10000);
    now_us += 25000;
    read_chunk(output);
    assert(state.mic_callback_late == 1 && state.mic_callback_max_gap_us == 25000);
    usb_audio_set_microphone_active(false);
    now_us += 1000000;
    usb_audio_set_microphone_active(true);
    read_chunk(output);
    assert(state.mic_callback_late == 1 && state.mic_callback_max_gap_us == 25000);
}

int main(void)
{
    /* Console diagnostics can precede USB initialization. Only the internal-RAM
     * mic ring uses a bounded spinlock; ISR reads never take the task mutex. */
    usb_audio_stats_t stats;
    usb_audio_get_stats(&stats);
    assert(mutex_creations == 1 && signal_creations == 1 && critical_entries == 2 && !stats.mounted);
    check_task_mutex_does_not_block_isr();
    check_prefill_and_tail();
    check_burst_backpressure();
    check_spurious_space_signal();
    check_cancelled_writer(close_and_reopen);
    check_cancelled_writer(stop_forwarding);
    check_cancelled_writer(disconnect_usb);
    check_cancelled_writer(usb_audio_clear);
    check_cancelled_writer(suspend_usb);
    check_speaker_path();
    check_suspend_resume();
    check_frame_trace();
    check_callback_timing();
    assert(isr_entries && mutex_creations == 1 && signal_creations == 1 && !critical_depth && !mutex_depth);
    puts("PASS: direct USB ISR PCM, task-mutex independence, backpressure, cancellation and prefill/tail");
}
