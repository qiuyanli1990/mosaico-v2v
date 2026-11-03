#include "aec_processor.h"
#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* Only the board audio worker owns the DSP and its buffers. */
static aec_handle_t *aec;
static size_t chunk;
static int16_t *mic;
static int16_t *ref;
static int16_t *out;

esp_err_t aec_processor_init(void)
{
    if (aec) {
        return ESP_OK;
    }
    aec = aec_create(16000, 4, 1, AEC_MODE_FD_HIGH_PERF);
    if (!aec) {
        return ESP_ERR_NO_MEM;
    }
    int size = aec_get_chunksize(aec);
    if (size <= 0 || size > 512) {
        aec_destroy(aec);
        aec = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    chunk = (size_t)size;
    if (!mic) {
        mic = heap_caps_aligned_alloc(16, 512 * sizeof(*mic), MALLOC_CAP_8BIT);
        ref = heap_caps_aligned_alloc(16, 512 * sizeof(*ref), MALLOC_CAP_8BIT);
        out = heap_caps_aligned_alloc(16, 512 * sizeof(*out), MALLOC_CAP_8BIT);
    }
    if (!mic || !ref || !out) {
        heap_caps_free(mic);
        heap_caps_free(ref);
        heap_caps_free(out);
        mic = ref = out = NULL;
        aec_destroy(aec);
        aec = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI("AEC", "48 kHz Mic/Ref -> 16 kHz AEC, chunk=%u; complete frames only", (unsigned)chunk);
    return ESP_OK;
}

esp_err_t aec_processor_reset(void)
{
    if (aec) {
        aec_destroy(aec);
        aec = NULL;
    }
    return aec_processor_init();
}

size_t aec_processor_frame_samples(void)
{
    return chunk * 3;
}

esp_err_t aec_processor_process_48k(const int16_t *microphone, const int16_t *reference,
                                    int16_t *output, size_t count)
{
    if (!aec || !microphone || !reference || !output || count != chunk * 3) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Identical 3:1 conversion on Mic and actual DAC reference. No FIFO wait,
     * no read-side zero padding: each input frame produces exactly one frame. */
    for (size_t i = 0; i < chunk; ++i) {
        size_t at = i * 3;
        mic[i] = ((int32_t)microphone[at] + microphone[at + 1] + microphone[at + 2]) / 3;
        ref[i] = ((int32_t)reference[at] + reference[at + 1] + reference[at + 2]) / 3;
    }
    aec_process(aec, mic, ref, out);
    for (size_t i = 0; i < chunk; ++i) {
        output[i * 3] = output[i * 3 + 1] = output[i * 3 + 2] = out[i];
    }
    return ESP_OK;
}
