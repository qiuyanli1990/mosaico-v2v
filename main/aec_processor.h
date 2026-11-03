#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Single worker API. Mic and Ref are simultaneous 48 kHz mono S16 PCM.
 * Process exactly frame_samples(); callers retain partial frames themselves. */
esp_err_t aec_processor_init(void);
esp_err_t aec_processor_reset(void);
size_t aec_processor_frame_samples(void);
esp_err_t aec_processor_process_48k(const int16_t *mic, const int16_t *ref,
                                    int16_t *out, size_t count);
