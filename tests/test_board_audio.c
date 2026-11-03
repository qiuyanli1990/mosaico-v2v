/* Real board audio workers with deterministic DMA/RTOS/codec shims.
 * The DSP stub is identity: these tests verify PCM ownership and time/routing,
 * not acoustic echo attenuation. Included to exercise private worker loops. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_audio.c"

static jmp_buf worker_exit;
static int64_t now_us;
static dma_frame_t events[1024];
static size_t event_head, event_count;
static unsigned creates, destroys, processed_frames;
static unsigned fail_creates;
static int16_t processed_ref[512];
static int16_t first_ref[512];
static unsigned delay_action;
static int16_t written[DMA_SAMPLES];
static int output_error;

int64_t esp_timer_get_time(void) { return now_us; }
void crash_trace_mark(crash_trace_slot_t slot, crash_trace_stage_t stage, uint32_t detail)
{ (void)slot; (void)stage; (void)detail; }
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps)
{ (void)caps; return aligned_alloc(alignment, size); }
void heap_caps_free(void *p) { free(p); }
static aec_handle_t dsp;
aec_handle_t *aec_create(int rate, int length, int channels, int mode)
{
    assert(rate == 16000 && length == 4 && channels == 1 && mode == 6);
    ++creates;
    if (fail_creates) {
        --fail_creates;
        return NULL;
    }
    return &dsp;
}
void aec_destroy(aec_handle_t *p) { assert(p == &dsp); ++destroys; }
int aec_get_chunksize(aec_handle_t *p) { (void)p; return 512; }
void aec_process(const aec_handle_t *p, int16_t *mic, int16_t *ref, int16_t *out)
{
    (void)p;
    ++processed_frames;
    if (processed_frames == 1) memcpy(first_ref, ref, sizeof(first_ref));
    memcpy(processed_ref, ref, sizeof(processed_ref));
    memcpy(out, mic, 512 * sizeof(*out));
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout)
{ (void)mutex; (void)timeout; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; return pdTRUE; }
QueueHandle_t xQueueCreate(unsigned count, size_t size)
{ (void)count; assert(size == sizeof(dma_frame_t)); return (void *)2; }
int xQueueSendFromISR(QueueHandle_t queue, const void *item, BaseType_t *wake)
{
    (void)queue;
    assert(event_count < 1024);
    events[event_count++] = *(const dma_frame_t *)item;
    *wake = pdFALSE;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t queue, void *item, unsigned timeout)
{
    (void)queue; (void)timeout;
    if (event_head == event_count) longjmp(worker_exit, 1);
    dma_frame_t *frame = &events[event_head++];
    now_us = (frame->start + DMA_SAMPLES) * 1000000 / BOARD_AUDIO_RATE;
    memcpy(item, frame, sizeof(*frame));
    return pdTRUE;
}
void vTaskDelay(unsigned ticks)
{
    now_us += ticks * 1000;
    if (delay_action == 1) {
        delay_action = 0;
        board_audio_set_route(1);
        board_audio_set_route(0); /* away and back during a partial read */
    } else if (delay_action == 2) {
        delay_action = 0;
        board_audio_set_output_enabled(0, false); /* unblock a full writer */
    } else if (output_error) {
        longjmp(worker_exit, 1);
    }
}
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t p, unsigned volume)
{ (void)p; (void)volume; return ESP_CODEC_DEV_OK; }
int esp_codec_dev_write(esp_codec_dev_handle_t p, void *data, int size)
{
    (void)p;
    assert(size == sizeof(written));
    memcpy(written, data, sizeof(written));
    if (output_error) return -1;
    longjmp(worker_exit, 1);
}

static void fill(int16_t *samples, size_t count, int16_t value)
{ for (size_t i = 0; i < count; ++i) samples[i] = value; }

static void reset_state(void)
{
    routing_lock = (void *)1;
    dma_events = (void *)2;
    event_head = event_count = 0;
    now_us = 0;
    processed_frames = 0;
    input_source = -1;
    route_generation = 0;
    route_since = 0;
    memset(output_enabled, 0, sizeof(output_enabled));
    pcm_ring_reset(&captured);
    pcm_timeline_reset(&reference);
    for (unsigned i = 0; i < BOARD_AUDIO_SOURCES; ++i) {
        inputs[i] = (pcm_ring_t){.samples = input_storage[i], .capacity = CAPTURE_SAMPLES};
        outputs[i] = (pcm_ring_t){.samples = output_storage[i], .capacity = PLAYBACK_SAMPLES};
    }
    atomic_store(&requested_volume, 70);
    atomic_store(&dma_event_drops, 0);
    last_speech_us = 0;
    assert(aec_processor_reset() == ESP_OK);
}

static void add_dma(int64_t start, bool ref, int16_t value)
{
    dma_frame_t *frame = &events[event_count++];
    frame->start = start;
    frame->reference = ref;
    fill(frame->samples, DMA_SAMPLES, value);
}

static void run_input(void)
{ if (!setjmp(worker_exit)) input_task(NULL); }
static void run_output(void)
{ if (!setjmp(worker_exit)) output_task(NULL); }

static void test_dma_slots(void)
{
    reset_state();
    dma_clock_t clock = {0};
    int16_t stereo[DMA_SAMPLES * 2];
    for (unsigned i = 0; i < DMA_SAMPLES; ++i) {
        stereo[i * 2] = (int16_t)(1000 + i);
        stereo[i * 2 + 1] = -500;
    }
    i2s_event_data_t event = {.dma_buf = stereo, .size = sizeof(stereo)};
    now_us = 5000;
    dma_completed(NULL, &event, &clock);
    assert(event_count == 1 && events[0].start == 0);
    for (unsigned i = 0; i < DMA_SAMPLES; ++i) assert(events[0].samples[i] == (int16_t)(1000 + i));
    int16_t mono[DMA_SAMPLES];
    fill(mono, DMA_SAMPLES, 1234);
    event.dma_buf = mono;
    event.size = sizeof(mono);
    now_us = 10000;
    dma_completed(NULL, &event, &clock);
    assert(event_count == 2 && events[1].start == 240 && events[1].samples[239] == 1234);
    event.size = 1;
    dma_completed(NULL, &event, &clock);
    assert(event_count == 2 && atomic_load(&dma_event_drops) == 1);
}

static void test_mic_and_reference(void)
{
    reset_state();
    /* Reverse PC capture works even when it has no local speaker output. */
    board_audio_set_route(1);
    for (unsigned i = 0; i < 30; ++i) {
        add_dma(i * DMA_SAMPLES, true, 0);
        add_dma(i * DMA_SAMPLES, false, 1234);
    }
    run_input();
    assert(processed_frames == 0 && inputs[1].count > 0 && inputs[0].count == 0);
    for (size_t i = 0; i < inputs[1].count; ++i) assert(input_storage[1][i] == 1234);
    assert(board_audio_input_level() > 0);

    reset_state();
    board_audio_set_route(0);
    /* Reference exists only for 10 ms. It must not hold 40+ ms of Mic. */
    for (unsigned i = 0; i < 30; ++i) {
        if (i < 2) add_dma(i * DMA_SAMPLES, true, 777);
        add_dma(i * DMA_SAMPLES, false, 4321);
    }
    run_input();
    assert(inputs[0].count > 0);
    assert(first_ref[0] == 777 && first_ref[159] == 777 && first_ref[160] == 0);
    for (size_t i = 0; i < inputs[0].count; ++i) assert(input_storage[0][i] == 4321);
    for (unsigned i = 0; i < 512; ++i) assert(processed_ref[i] == 0);

    reset_state();
    board_audio_set_route(0);
    /* A five-second-old reference cannot be paired with current capture. */
    add_dma(0, true, 777);
    for (unsigned i = 0; i < 30; ++i) add_dma(240000 + i * DMA_SAMPLES, false, 2222);
    run_input();
    assert(processed_frames == 0);
    assert(inputs[0].count > 0);

    reset_state();
    now_us = 10000;
    board_audio_set_route(0);
    for (unsigned i = 0; i < 12; ++i) {
        add_dma(i * DMA_SAMPLES, true, 0);
        add_dma(i * DMA_SAMPLES, false, i < 2 ? 1111 : 2222);
    }
    run_input();
    assert(inputs[0].count == 1536 - 480);
    for (size_t i = 0; i < inputs[0].count; ++i) assert(input_storage[0][i] == 2222);
}

static void test_routes_and_mixer(void)
{
    reset_state();
    int16_t a[DMA_SAMPLES], b[DMA_SAMPLES], out[DMA_SAMPLES];
    fill(a, DMA_SAMPLES, 20000);
    fill(b, DMA_SAMPLES, 20000);
    board_audio_set_route(0);
    pcm_ring_write(&inputs[0], a, DMA_SAMPLES);
    board_audio_set_route(1);
    assert(inputs[0].count == 0 && board_audio_read(0, out, DMA_SAMPLES, 0) == 0);
    pcm_ring_write(&inputs[1], b, DMA_SAMPLES);
    assert(board_audio_read(1, out, DMA_SAMPLES, 0) == DMA_SAMPLES);
    board_audio_set_route(-1);
    assert(inputs[0].count == 0 && inputs[1].count == 0);
    board_audio_set_route(0);
    pcm_ring_write(&inputs[0], a, DMA_SAMPLES / 2);
    delay_action = 1;
    assert(board_audio_read(0, out, DMA_SAMPLES, 10) == 0);

    board_audio_set_output_enabled(0, true);
    board_audio_set_output_enabled(1, true);
    assert(board_audio_write(0, a, DMA_SAMPLES));
    assert(board_audio_write(1, b, DMA_SAMPLES));
    run_output();
    for (unsigned i = 0; i < DMA_SAMPLES; ++i) assert(written[i] == INT16_MAX);
    /* Mixing/write completion itself cannot feed the reference. Only DMA TX
     * completion contains the sound that actually left the board. */
    assert(reference.count == 0 && !board_audio_is_speaking());
    output_error = 1;
    run_output();
    assert(reference.count == 0);
    output_error = 0;
    pcm_ring_write(&outputs[0], output_storage[0], outputs[0].capacity);
    delay_action = 2;
    assert(board_audio_write(0, a, DMA_SAMPLES));
    assert(outputs[0].count == 0);
}

static void test_gap_reset_coalescing(void)
{
    reset_state();
    unsigned before = creates;
    /* Several holes before a complete frame must not repeatedly rebuild the
     * DSP while the DMA queue is already catching up after a busy interval. */
    add_dma(0, false, 1000);
    add_dma(480, false, 1000);
    add_dma(960, false, 1000);
    add_dma(1440, true, 777);
    for (unsigned i = 0; i < 12; ++i) add_dma(1440 + i * DMA_SAMPLES, false, 1000);
    run_input();
    assert(creates == before + 1);
    assert(processed_frames > 0);

    reset_state();
    before = creates;
    fail_creates = 1;
    board_audio_set_route(0);
    add_dma(0, false, 1000);
    for (unsigned i = 0; i < 250; ++i) {
        add_dma(480 + i * DMA_SAMPLES, true, 777);
        add_dma(480 + i * DMA_SAMPLES, false, 1000);
    }
    run_input();
    /* Retry a failed allocation after one second. Mic still passes through,
     * rather than rebuilding the unavailable DSP every 32 ms. */
    assert(creates == before + 2);
    assert(inputs[0].count > 0 && processed_frames > 0);
}

static void test_dsp_idle_and_resume(void)
{
    reset_state();
    unsigned before = creates;
    board_audio_set_route(0);
    for (unsigned i = 0; i < 320; ++i) {
        int64_t start = i * DMA_SAMPLES;
        if (i == 2) start += DMA_SAMPLES; /* a gap before any playback */
        add_dma(start, true, i < 10 || i >= 290 ? 777 : 0);
        add_dma(start, false, 1234);
    }
    run_input();
    /* 1.6 s of Mic, but DSP only during two playback bursts + echo tails.
     * The initial gap resets once, idle frames never rebuild, and playback
     * resumes with one reset of history left over from the first burst. */
    assert(processed_frames > 0 && processed_frames < 25);
    assert(creates == before + 2);
    for (size_t i = 0; i < inputs[0].count; ++i) assert(input_storage[0][i] == 1234);

    reset_state();
    before = creates;
    add_dma(0, false, 1000);
    for (unsigned i = 0; i < 100; ++i) add_dma(480 + i * DMA_SAMPLES, false, 1000);
    run_input();
    assert(creates == before && processed_frames == 0);
}

int main(void)
{
    test_dma_slots();
    test_mic_and_reference();
    test_routes_and_mixer();
    test_gap_reset_coalescing();
    test_dsp_idle_and_resume();
    assert(creates > 0 && destroys > 0);
    puts("PASS: actual audio workers: Mic/ref time, DMA slots, no-ref speech, old-ref expiry, turn switching, PC reverse capture, DAC mixing/errors/cancellation, coalesced AEC reset/allocation backoff");
    return 0;
}
