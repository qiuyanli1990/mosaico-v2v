#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_descriptors.c"

static tusb_speed_t speed;
tusb_speed_t tud_speed_get(void) { return speed; }
static uint8_t feedback_function;
static uint32_t feedback_value;
bool tud_audio_n_fb_set(uint8_t func_id, uint32_t value)
{
    feedback_function = func_id;
    feedback_value = value;
    return true;
}

static unsigned le16(const uint8_t *bytes) { return bytes[0] | ((unsigned)bytes[1] << 8); }

static void assert_string(unsigned index, const char *expected)
{
    const uint16_t *descriptor = tud_descriptor_string_cb(index, 0x0409);
    assert(descriptor && (descriptor[0] >> 8) == TUSB_DESC_STRING);
    size_t length = ((descriptor[0] & 255) - 2) / 2;
    assert(length == strlen(expected));
    for (size_t i = 0; i < length; ++i) assert(descriptor[i + 1] == (unsigned char)expected[i]);
}

static void check_configuration(const uint8_t *actual, const uint8_t *original, uint8_t type,
                                bool high_speed)
{
    assert(actual[1] == type && le16(actual + 2) == CONFIG_TOTAL_LEN && actual[4] == 5);
    unsigned named_entities = 0, speaker_interfaces = 0, mic_interfaces = 0, endpoints = 0;
    bool control = false, speaker_stream = false;
    size_t offset = 0;
    while (offset < CONFIG_TOTAL_LEN) {
        const uint8_t *descriptor = actual + offset;
        unsigned length = descriptor[0];
        assert(length >= 2 && offset + length <= CONFIG_TOTAL_LEN);
        bool changed[256] = {false};
        if (descriptor[1] == TUSB_DESC_INTERFACE) {
            control = descriptor[2] == ITF_NUM_AUDIO_CONTROL;
            speaker_stream = descriptor[2] == ITF_NUM_AUDIO_STREAMING_SPK;
            if (descriptor[2] == ITF_NUM_AUDIO_STREAMING_SPK) {
                assert_string(descriptor[8], "mosaico");
                ++speaker_interfaces;
            }
            if (descriptor[2] == ITF_NUM_AUDIO_STREAMING_MIC) {
                assert_string(descriptor[8], "mosaico");
                ++mic_interfaces;
            }
        } else if (control && descriptor[1] == TUSB_DESC_CS_INTERFACE &&
                   (descriptor[2] == AUDIO_CS_AC_INTERFACE_INPUT_TERMINAL ||
                    descriptor[2] == AUDIO_CS_AC_INTERFACE_OUTPUT_TERMINAL ||
                    descriptor[2] == AUDIO_CS_AC_INTERFACE_FEATURE_UNIT)) {
            unsigned entity = descriptor[3];
            bool speaker = entity == 1 || entity == 2 || entity == 3;
            assert(speaker || entity == 0x11 || entity == 0x12 || entity == 0x13);
            assert_string(descriptor[length - 1], "mosaico");
            changed[length - 1] = true;
            if (entity == UAC2_ENTITY_SPK_INPUT_TERMINAL) {
                assert(descriptor[8] == 2 && le16(descriptor + 9) == 3 && le16(descriptor + 11) == 0);
                for (unsigned i = 9; i < 13; ++i) changed[i] = true;
            }
            ++named_entities;
        } else if (speaker_stream && descriptor[1] == TUSB_DESC_CS_INTERFACE &&
                   descriptor[2] == AUDIO_CS_AS_INTERFACE_AS_GENERAL) {
            assert(descriptor[10] == 2 && le16(descriptor + 11) == 3 && le16(descriptor + 13) == 0);
            for (unsigned i = 11; i < 15; ++i) changed[i] = true;
        } else if (descriptor[1] == TUSB_DESC_ENDPOINT) {
            switch (descriptor[2]) {
            case 0x01:
                assert(descriptor[3] == 0x05 && le16(descriptor + 4) == (high_speed ? 28 : 196));
                assert(descriptor[6] == 1);
                changed[4] = changed[5] = changed[6] = true;
                endpoints |= 1;
                break;
            case 0x81:
                assert(descriptor[3] == 0x11 && le16(descriptor + 4) == (high_speed ? 4 : 3));
                assert(descriptor[6] == (high_speed ? 4 : 1));
                changed[4] = changed[5] = changed[6] = true;
                endpoints |= 2;
                break;
            case 0x82:
                assert(le16(descriptor + 4) == 98 && descriptor[6] == (high_speed ? 4 : 1));
                changed[6] = true;
                endpoints |= 4;
                break;
            case 0x83: break;
            case 0x04: case 0x84:
                assert(le16(descriptor + 4) == (high_speed ? 512 : 64));
                break;
            default: assert(false);
            }
        }
        /* Preserve every unrelated byte of the upstream audio/CDC topology. */
        for (unsigned i = 0; i < length; ++i) {
            if ((offset == 0 && i == 1) || changed[i]) continue;
            assert(descriptor[i] == original[offset + i]);
        }
        offset += length;
    }
    assert(offset == CONFIG_TOTAL_LEN && named_entities == 6);
    assert(speaker_interfaces == 2 && mic_interfaces == 2 && endpoints == 7);
}

int main(void)
{
    const tusb_desc_device_t *device = (const tusb_desc_device_t *)tud_descriptor_device_cb();
    assert(device->idVendor == 0x303A && device->idProduct == 0x4012 && device->bcdDevice == 0x0105);
    assert(CFG_TUD_AUDIO_FUNC_1_N_AS_INT == 2);
    assert(CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ >= 1920);
    assert(CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP && CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION);
    assert_string(device->iProduct, "mosaico");
    assert_string(device->iManufacturer, "mosaico");
    assert_string(4, "mosaico");
    assert(!tud_descriptor_string_cb(255, 0));
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        speed = TUSB_SPEED_HIGH;
        const uint8_t *configuration = tud_descriptor_configuration_cb(0);
        check_configuration(configuration, high_speed_configuration, TUSB_DESC_CONFIGURATION, true);
        check_configuration(tud_descriptor_other_speed_configuration_cb(0), full_speed_configuration,
                            TUSB_DESC_OTHER_SPEED_CONFIG, false);
        check_configuration(configuration, high_speed_configuration, TUSB_DESC_CONFIGURATION, true);
        audio_feedback_params_t params = {0};
        tud_audio_feedback_params_cb(0, 1, &params);
        assert(params.sample_freq == 48000 && params.method == AUDIO_FEEDBACK_METHOD_DISABLED);
        assert(feedback_function == 0 && feedback_value == 0x00060000);
        speed = TUSB_SPEED_FULL;
        check_configuration(tud_descriptor_configuration_cb(0), full_speed_configuration, TUSB_DESC_CONFIGURATION, false);
        check_configuration(tud_descriptor_other_speed_configuration_cb(0), high_speed_configuration,
                            TUSB_DESC_OTHER_SPEED_CONFIG, true);
        tud_audio_feedback_params_cb(0, 1, &params);
        assert(params.sample_freq == 48000 && params.method == AUDIO_FEEDBACK_METHOD_FIFO_COUNT);
        assert(feedback_value == 0x00300000);
    }
    puts("PASS: actual USB callbacks: HS OUT 28 B/125 us, mono IN 98 B/1 ms, fixed HS feedback at 1 ms, FS/other-speed descriptors, FL/FR, two AS interfaces, CDC and names");
    return 0;
}
