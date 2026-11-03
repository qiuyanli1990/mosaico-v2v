#pragma once
#include <stdbool.h>
#include "esp_capture_audio_src_if.h"

typedef enum {
    USB_CAPTURE_SOURCE_SPEAKER,
    USB_CAPTURE_SOURCE_MICROPHONE,
} usb_capture_source_id_t;

void usb_capture_set_enabled(unsigned source, bool enabled);
void usb_capture_set_portable_mode(bool portable);
void usb_capture_set_portable_input(int media_source);
unsigned usb_capture_get_portable_level(void);
esp_capture_audio_src_if_t *usb_capture_source(usb_capture_source_id_t source);

