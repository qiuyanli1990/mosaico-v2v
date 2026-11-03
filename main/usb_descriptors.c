#include <string.h>

#include "tusb.h"
#include "uac_descriptors.h"
#include "usb_descriptors.h"

#define DEVICE_VID 0x303A
#define DEVICE_PID 0x4012
#define CDC_NOTIFY_EP 0x83
#define CDC_OUT_EP 0x04
#define CDC_IN_EP 0x84
#define AUDIO_OUT_EP 0x01
#define AUDIO_FB_EP 0x81
#define AUDIO_IN_EP 0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN + TUD_CDC_DESC_LEN)

static tusb_desc_device_t const device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = DEVICE_VID,
    .idProduct = DEVICE_PID,
    .bcdDevice = MOSAICO_USB_DEVICE_REVISION,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static tusb_desc_device_qualifier_t const device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
};

static uint8_t const high_speed_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 7, CDC_NOTIFY_EP, 8, CDC_OUT_EP, CDC_IN_EP, 512),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 4, AUDIO_OUT_EP, AUDIO_IN_EP, AUDIO_FB_EP),
};

static uint8_t const full_speed_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x80, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 7, CDC_NOTIFY_EP, 8, CDC_OUT_EP, CDC_IN_EP, 64),
    TUD_AUDIO_DESCRIPTOR(ITF_NUM_AUDIO_CONTROL, 4, AUDIO_OUT_EP, AUDIO_IN_EP, AUDIO_FB_EP),
};

static uint8_t named_configuration[CONFIG_TOTAL_LEN];
static uint8_t other_speed_configuration[CONFIG_TOTAL_LEN];

static void copy_named_configuration(uint8_t *out, const uint8_t *source, uint8_t type,
                                     bool high_speed)
{
    memcpy(out, source, CONFIG_TOTAL_LEN);
    out[1] = type;
    bool audio_control = false;
    bool speaker_stream = false;
    /* Name the UAC entities too: hosts can use iTerminal/iFeature for endpoint names. */
    for (size_t offset = 0; offset + 2 <= CONFIG_TOTAL_LEN;) {
        uint8_t *descriptor = out + offset;
        size_t length = descriptor[0];
        if (length < 2 || offset + length > CONFIG_TOTAL_LEN) break;
        if (descriptor[1] == TUSB_DESC_INTERFACE && length >= 9) {
            audio_control = descriptor[5] == TUSB_CLASS_AUDIO && descriptor[6] == AUDIO_SUBCLASS_CONTROL;
            speaker_stream = descriptor[2] == ITF_NUM_AUDIO_STREAMING_SPK;
        } else if (audio_control && descriptor[1] == TUSB_DESC_CS_INTERFACE && length >= 4 &&
                   (descriptor[2] == AUDIO_CS_AC_INTERFACE_INPUT_TERMINAL ||
                    descriptor[2] == AUDIO_CS_AC_INTERFACE_OUTPUT_TERMINAL ||
                    descriptor[2] == AUDIO_CS_AC_INTERFACE_FEATURE_UNIT)) {
            switch (descriptor[3]) {
            case UAC2_ENTITY_SPK_INPUT_TERMINAL:
                /* Two physical channels with explicit front-left/front-right positions. */
                descriptor[9] = AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT;
                descriptor[10] = descriptor[11] = descriptor[12] = 0;
                descriptor[length - 1] = 5;
                break;
            case UAC2_ENTITY_SPK_FEATURE_UNIT:
            case UAC2_ENTITY_SPK_OUTPUT_TERMINAL:
                descriptor[length - 1] = 5;
                break;
            case UAC2_ENTITY_MIC_INPUT_TERMINAL:
            case UAC2_ENTITY_MIC_FEATURE_TERMINAL:
            case UAC2_ENTITY_MIC_OUTPUT_TERMINAL:
                descriptor[length - 1] = 6;
                break;
            }
        } else if (speaker_stream && descriptor[1] == TUSB_DESC_CS_INTERFACE &&
                   descriptor[2] == AUDIO_CS_AS_INTERFACE_AS_GENERAL && length >= 16) {
            descriptor[11] = AUDIO_CHANNEL_CONFIG_FRONT_LEFT | AUDIO_CHANNEL_CONFIG_FRONT_RIGHT;
            descriptor[12] = descriptor[13] = descriptor[14] = 0;
        } else if (descriptor[1] == TUSB_DESC_ENDPOINT && length >= 7) {
            unsigned packet_size = descriptor[4] | ((unsigned)descriptor[5] << 8);
            if (descriptor[2] == AUDIO_OUT_EP) {
                /* HS OUT: six stereo frames per microframe, plus one for feedback drift.
                 * Keep the component's millisecond-sized FIFO and callback buffers. */
                packet_size = (CONFIG_UAC_SAMPLE_RATE / (high_speed ? 8000 : 1000) + 1) *
                              CONFIG_UAC_SPEAKER_CHANNEL_NUM * CONFIG_UAC_BYTES_PER_SAMPLE;
                descriptor[6] = 1;
            } else if (descriptor[2] == AUDIO_IN_EP) {
                /* MIC remains on a 1 ms cadence; it is mono in this application. */
                descriptor[6] = high_speed ? 4 : 1;
            } else if (descriptor[2] == AUDIO_FB_EP) {
                packet_size = !high_speed && CONFIG_UAC_SUPPORT_MACOS ? 3 : 4;
                /* Feedback has its own 1 ms cadence, independent of HS OUT.
                 * Match TinyUSB's speaker reference and allow the DWC2 retry
                 * window to span a complete host feedback polling interval. */
                descriptor[6] = high_speed ? 4 : 1;
            }
            descriptor[4] = packet_size & 255;
            descriptor[5] = packet_size >> 8;
        }
        offset += length;
    }
}

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&device_descriptor;
}

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
    return (uint8_t const *)&device_qualifier;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    bool high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
    const uint8_t *configuration = high_speed
        ? high_speed_configuration : full_speed_configuration;
    copy_named_configuration(named_configuration, configuration, TUSB_DESC_CONFIGURATION, high_speed);
    return named_configuration;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    bool high_speed = tud_speed_get() != TUSB_SPEED_HIGH;
    const uint8_t *configuration = high_speed
        ? high_speed_configuration : full_speed_configuration;
    copy_named_configuration(other_speed_configuration, configuration, TUSB_DESC_OTHER_SPEED_CONFIG,
                             high_speed);
    return other_speed_configuration;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf,
                                 audio_feedback_params_t *params)
{
    (void)alt_itf;
    bool high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
    params->sample_freq = CONFIG_UAC_SAMPLE_RATE;
    params->method = high_speed ? AUDIO_FEEDBACK_METHOD_DISABLED : AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    /* HS uses explicit, fixed 16.16 feedback: 48 kHz / 8000 = 0x00060000.
     * Seed it when the stream opens instead of waiting for the first OUT packet. */
    tud_audio_n_fb_set(func_id, ((uint32_t)CONFIG_UAC_SAMPLE_RATE << 16) /
                              (high_speed ? 8000 : 1000));
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static char const *const strings[] = {
        "",
        MOSAICO_USB_DEVICE_NAME,
        MOSAICO_USB_DEVICE_NAME,
        "12345678",
        MOSAICO_USB_DEVICE_NAME,
        MOSAICO_USB_DEVICE_NAME,
        MOSAICO_USB_DEVICE_NAME,
        "Serial Console",
    };
    static uint16_t descriptor[32];

    if (index >= sizeof(strings) / sizeof(strings[0])) {
        return NULL;
    }
    if (index == 0) {
        descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        descriptor[1] = 0x0409;
        return descriptor;
    }
    size_t length = strlen(strings[index]);
    if (length > 31) {
        length = 31;
    }
    for (size_t i = 0; i < length; ++i) {
        descriptor[i + 1] = (uint8_t)strings[index][i];
    }
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * length + 2));
    return descriptor;
}
