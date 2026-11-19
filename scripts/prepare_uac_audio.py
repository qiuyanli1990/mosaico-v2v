#!/usr/bin/env python3
"""Prepare an ISR-fed microphone bridge for the pinned UAC component."""
import argparse
import hashlib
from pathlib import Path


MIC_STOP = """static void stop_microphone(void)
{
    UAC_ENTER_CRITICAL();
    s_uac_device->mic_active = false;
    s_uac_device->mic_data_size = 0;
    ++s_uac_device->mic_generation;
    UAC_EXIT_CRITICAL();
    usb_audio_set_microphone_active(false);
}
"""

MIC_TX = """bool tud_audio_tx_done_isr(uint8_t rhport, uint16_t n_bytes_sent, uint8_t func_id, uint8_t ep_in, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_sent;
    (void)ep_in;
    (void)cur_alt_setting;
    uint32_t started = (uint32_t)esp_timer_get_time();
    tu_fifo_t *fifo = tud_audio_n_get_ep_in_ff(func_id);
    UAC_ENTER_CRITICAL();
    if (s_uac_device->mic_active) {
        size_t queued = tu_fifo_count(fifo);
        size_t target = tu_fifo_depth(fifo) / 2;
        target -= target % sizeof(int16_t);
        if (queued < target) {
            size_t bytes = target - queued;
            bytes -= bytes % sizeof(int16_t);
            usb_audio_read_microphone_isr((int16_t *)s_uac_device->mic_buf_read,
                                          bytes / sizeof(int16_t));
            tud_audio_n_write(func_id, s_uac_device->mic_buf_read, bytes);
        }
    }
    UAC_EXIT_CRITICAL();
    usb_uac_trace_mic_feed((uint32_t)esp_timer_get_time() - started);
    return true;
}"""


def replace_function(source, signature, digest, replacement):
    if source.count(signature) != 1:
        raise ValueError("Pinned UAC function changed: " + signature)
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    if hashlib.sha256(source[start:end].encode()).hexdigest() != digest:
        raise ValueError("Pinned UAC function changed; review microphone patch: " + signature)
    return source[:start] + replacement + source[end:]


def prepare(source: str) -> str:
    source = replace_function(source, "bool tud_audio_tx_done_isr(",
        "5ffe06b4e968291ba4f25a685215f4c31e68458234085e2fb6ec15be54c2db8b", MIC_TX)
    source = replace_function(source, "static void usb_mic_task(",
        "71c9a9240c1aad387c4bc86369027639ae5c0c58c2a57e531b9e5e1ed1e443d5", "")
    changes = [
        ('        xTaskNotifyGive(s_uac_device->mic_task_handle);', ''),
        ('    ret_val = xTaskCreatePinnedToCore(usb_mic_task, "usb_mic_task", 4096, NULL, CONFIG_UAC_MIC_TASK_PRIORITY,\n'
         '                                      &s_uac_device->mic_task_handle, CONFIG_UAC_MIC_TASK_CORE == -1 ? tskNO_AFFINITY : CONFIG_UAC_MIC_TASK_CORE);\n'
         '    ESP_RETURN_ON_FALSE(ret_val == pdPASS, ESP_FAIL, TAG, "Failed to create usb_mic task");',
         '    /* USB IN refills directly from the internal-SRAM application ring. */'),
        ('#include "usb_device_uac.h"', '#include "usb_device_uac.h"\n#include "usb_audio.h"\n#include "usb_uac_trace.h"\n#include "esp_heap_caps.h"'),
        ('    s_uac_device = calloc(1, sizeof(uac_device_t));',
         '    /* USB ISR reads these buffers and metadata; keep them off PSRAM. */\n'
         '    s_uac_device = heap_caps_calloc(1, sizeof(uac_device_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);'),
        ('    int mic_data_size;', '    int mic_data_size;\n    uint32_t mic_generation;'),
        ('#define UAC_EXIT_CRITICAL()     portEXIT_CRITICAL(&s_mux)',
         '#define UAC_EXIT_CRITICAL()     portEXIT_CRITICAL(&s_mux)\n\n' + MIC_STOP),
        ('\n    s_uac_device->mic_active = false;', '\n    stop_microphone();'),
        ('        s_uac_device->mic_data_size = 0;\n        s_uac_device->mic_active = false;',
         '        stop_microphone();'),
        ('    ESP_LOGI(TAG, "USB unmounted");', '    stop_microphone();\n    ESP_LOGI(TAG, "USB unmounted");'),
        ('        s_uac_device->mic_data_size = 0;\n'
         '        s_uac_device->mic_resolution = mic_resolutions_per_format[alt - 1];\n'
         '        s_uac_device->mic_active = true;\n'
         '        s_uac_device->mic_bytes_per_ms = s_uac_device->current_sample_rate / 1000 * MIC_CHANNEL_NUM * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX;',
         '        usb_audio_set_microphone_active(true);\n'
         '        UAC_ENTER_CRITICAL();\n'
         '        s_uac_device->mic_data_size = 0;\n'
         '        ++s_uac_device->mic_generation;\n'
         '        s_uac_device->mic_resolution = mic_resolutions_per_format[alt - 1];\n'
         '        s_uac_device->mic_bytes_per_ms = s_uac_device->current_sample_rate / 1000 * MIC_CHANNEL_NUM * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX;\n'
         '        s_uac_device->mic_active = true;\n'
         '        UAC_EXIT_CRITICAL();'),
    ]
    # Apply lifecycle replacements before inserting the helper with the same field names.
    helper = changes.pop(5)
    for original, replacement in changes:
        expected = 2 if original == '\n    s_uac_device->mic_active = false;' else 1
        if source.count(original) != expected:
            raise ValueError("Pinned UAC lifecycle changed; review microphone patch: " + original)
        source = source.replace(original, replacement)
    if source.count(helper[0]) != 1:
        raise ValueError("Pinned UAC critical-section definitions changed")
    return source.replace(*helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    content = prepare(args.source.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)


if __name__ == "__main__":
    main()
