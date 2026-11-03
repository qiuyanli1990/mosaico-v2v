#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pcm_ring.h"
#include "pcm_timeline.h"
#include "translation_protocol.h"

static void test_ring(void)
{
    int16_t storage[7];
    pcm_ring_t ring = {.samples = storage, .capacity = 7};
    int16_t reference[7];
    size_t reference_count = 0;
    uint64_t dropped = 0;
    unsigned random = 1234;
    for (unsigned operation = 0; operation < 20000; ++operation) {
        random = random * 1664525U + 1013904223U;
        if (random % 31 == 0) {
            pcm_ring_reset(&ring);
            reference_count = 0;
        } else if (random & 1) {
            size_t count = random % 23;
            int16_t values[23];
            for (size_t i = 0; i < count; ++i) {
                values[i] = (int16_t)(operation + i);
                if (reference_count == 7) {
                    memmove(reference, reference + 1, 6 * sizeof(*reference));
                    --reference_count;
                    ++dropped;
                }
                reference[reference_count++] = values[i];
            }
            pcm_ring_write(&ring, values, count);
        } else {
            size_t requested = random % 11;
            int16_t values[11] = {0};
            size_t expected = requested < reference_count ? requested : reference_count;
            assert(pcm_ring_read(&ring, values, requested) == expected);
            assert(memcmp(values, reference, expected * sizeof(*reference)) == 0);
            memmove(reference, reference + expected, (reference_count - expected) * sizeof(*reference));
            reference_count -= expected;
        }
        assert(ring.count == reference_count);
        assert(ring.dropped == dropped);
        assert(ring.count <= ring.capacity);
    }
}

static void test_audio_timeline(void)
{
    int16_t storage[8] = {0};
    pcm_timeline_t timeline = {.samples = storage, .capacity = 8};
    int16_t first[] = {10, 20, 30, 40};
    int16_t second[] = {50, 60};
    int16_t output[6] = {0};
    pcm_timeline_write(&timeline, 100, first, 4);
    assert(pcm_timeline_read(&timeline, 99, output, 6) == 4);
    assert(output[0] == 0 && output[1] == 10 && output[4] == 40 && output[5] == 0);
    pcm_timeline_write(&timeline, 104, second, 2);
    memset(output, 0, sizeof(output));
    assert(pcm_timeline_read(&timeline, 100, output, 6) == 6);
    assert(output[4] == 50 && output[5] == 60);
    /* Wrap retains the latest timestamps and expires the overwritten head. */
    pcm_timeline_write(&timeline, 106, first, 4);
    assert(pcm_timeline_read(&timeline, 100, output, 6) == 4);
    assert(output[0] == 0 && output[1] == 0 && output[2] == 30 && output[5] == 60);
    pcm_timeline_write(&timeline, 1000, first, 4);
    memset(output, 0, sizeof(output));
    assert(pcm_timeline_read(&timeline, 100, output, 1) == 0 && output[0] == 0);
    int16_t mixed[3];
    int16_t left[] = {INT16_MAX, INT16_MIN, 100};
    int16_t right[] = {100, -100, -200};
    pcm_mix_s16(mixed, left, right, 3);
    assert(mixed[0] == INT16_MAX && mixed[1] == INT16_MIN && mixed[2] == -100);
}

static const cJSON *field(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    assert(value);
    return value;
}

static void test_join_contract(void)
{
    translation_request_t request = {
        .channel = "mosaico_unique", .device_uid = "12345", .agent_uid = "88888",
        .voice_id = "male-qn-qingse", .source_language = "zh-CN",
        .target_language = "en-US", .tts_enabled = true, .idle_timeout = 300,
    };
    char *text = translation_join_json(&request);
    assert(text);
    cJSON *json = cJSON_Parse(text);
    assert(json);
    assert(strcmp(field(json, "preset")->valuestring, "v2vt_base") == 0);
    const cJSON *properties = field(json, "properties");
    assert(strcmp(field(properties, "channel")->valuestring, request.channel) == 0);
    assert(!cJSON_GetObjectItemCaseSensitive(properties, "token"));
    assert(!cJSON_GetObjectItemCaseSensitive(properties, "agent_rtm_uid"));
    assert(strcmp(field(properties, "agent_rtc_uid")->valuestring, "88888") == 0);
    assert(cJSON_IsTrue(field(properties, "enable_string_uid")));
    const cJSON *uids = field(properties, "remote_rtc_uids");
    assert(cJSON_GetArraySize(uids) == 1);
    assert(strcmp(cJSON_GetArrayItem(uids, 0)->valuestring, "12345") == 0);
    assert(cJSON_IsFalse(field(field(properties, "advanced_features"), "enable_rtm")));
    assert(strcmp(field(field(properties, "parameters"), "data_channel")->valuestring, "datastream") == 0);
    assert(strcmp(field(field(properties, "asr"), "language")->valuestring, "zh-CN") == 0);
    assert(strcmp(field(field(properties, "translation"), "language")->valuestring, "en-US") == 0);
    assert(cJSON_IsTrue(field(field(properties, "tts"), "enable")));
    assert(strcmp(field(field(properties, "tts"), "voice_id")->valuestring, "fm_02_male-qn-qingse") == 0);
    cJSON_Delete(json);
    cJSON_free(text);
    request.voice_id = "fm_02_English_Graceful_Lady";
    text = translation_join_json(&request);
    assert(text);
    json = cJSON_Parse(text);
    assert(strcmp(field(field(field(json, "properties"), "tts"), "voice_id")->valuestring,
                  request.voice_id) == 0);
    cJSON_Delete(json);
    cJSON_free(text);
    request.voice_id = "fm_02_fm_02_English_Graceful_Lady";
    assert(!translation_join_json(&request));
    request.voice_id = NULL;
    assert(!translation_join_json(&request));
    request.voice_id = "fm_02_English_Graceful_Lady";
    request.agent_uid = request.device_uid;
    assert(!translation_join_json(&request));
    request.agent_uid = "4294967296";
    assert(!translation_join_json(&request));
    request.agent_uid = "agent/unsafe";
    assert(!translation_join_json(&request));
    request.agent_uid = "88888";
    request.target_language = "sq-AL";
    request.tts_enabled = false;
    request.voice_id = NULL;
    text = translation_join_json(&request);
    assert(text);
    json = cJSON_Parse(text);
    assert(cJSON_IsFalse(field(field(field(json, "properties"), "tts"), "enable")));
    assert(!cJSON_GetObjectItemCaseSensitive(field(field(json, "properties"), "tts"), "voice_id"));
    cJSON_Delete(json);
    cJSON_free(text);
    request.tts_enabled = true;
    assert(!translation_join_json(&request));
    assert(translation_language_index("zh-CN") < TRANSLATION_LANGUAGE_COUNT);
    assert(translation_tts_language_count() == 35);
}

static void test_voices(void)
{
    char formatted[TRANSLATION_VOICE_ID_SIZE];
    assert(translation_format_voice_id("English_Graceful_Lady", formatted, sizeof(formatted)));
    assert(strcmp(formatted, "fm_02_English_Graceful_Lady") == 0);
    assert(translation_format_voice_id(formatted, formatted, sizeof(formatted)));
    assert(strcmp(formatted, "fm_02_English_Graceful_Lady") == 0);
    assert(!translation_format_voice_id(NULL, formatted, sizeof(formatted)));
    assert(!translation_format_voice_id("", formatted, sizeof(formatted)));
    assert(!translation_format_voice_id("fm_02_", formatted, sizeof(formatted)));
    assert(!translation_format_voice_id("fm_02_fm_02_voice", formatted, sizeof(formatted)));
    assert(!translation_format_voice_id("voice", NULL, sizeof(formatted)));
    assert(!translation_format_voice_id("voice", formatted, 11));
    assert(translation_format_voice_id("voice", formatted, 12));
    char too_long[TRANSLATION_VOICE_ID_SIZE];
    memset(too_long, 'v', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    assert(!translation_format_voice_id(too_long, formatted, sizeof(formatted)));

    size_t supported = 0;
    for (size_t i = 0; i < TRANSLATION_LANGUAGE_COUNT; ++i) {
        const translation_language_t *language = translation_language_at(i);
        size_t count = translation_voice_count_for_language(language->code);
        assert((count > 0) == language->tts_supported);
        supported += count > 0;
        size_t dropdown_size = 0;
        for (size_t option = 0; option < count; ++option) {
            const translation_voice_t *voice = translation_voice_for_language(language->code, option);
            assert(voice && voice->name[0]);
            assert(translation_format_voice_id(voice->id, formatted, sizeof(formatted)));
            assert(strcmp(voice->id, formatted) == 0);
            dropdown_size += strlen(voice->name) + 1;
            for (size_t earlier = 0; earlier < option; ++earlier)
                assert(strcmp(voice->id, translation_voice_for_language(language->code, earlier)->id) != 0);
        }
        assert(dropdown_size < 512);
        assert(!translation_voice_for_language(language->code, count));
    }
    assert(supported == 35);
    const char *native_languages[] = {"zh-CN", "ja-JP", "ko-KR", "en-US", "de-DE", "fr-FR", "es-ES",
                                      "pt-BR", "ru-RU", "ar-SA", "hi-IN", "id-ID", "th-TH", "vi-VN"};
    for (size_t i = 0; i < sizeof(native_languages) / sizeof(native_languages[0]); ++i)
        assert(translation_voice_count_for_language(native_languages[i]) > 0);
    assert(strcmp(translation_voice_for_language("en-US", 0)->id, "fm_02_English_Graceful_Lady") == 0);
    assert(strcmp(translation_voice_for_language("zh-CN", 0)->id, "fm_02_male-qn-qingse") == 0);
    assert(translation_voice_count_for_language("en-SG") == translation_voice_count_for_language("en-US"));
    assert(translation_voice_count_for_language("zh-SG") == translation_voice_count_for_language("zh-CN"));
    assert(translation_voice_count_for_language("ms-SG") == translation_voice_count_for_language("ms-MY"));
    assert(strstr(translation_voice_for_language("ms-MY", 0)->name, "multilingual"));
    assert(!translation_voice_count_for_language("tl-PH"));
    assert(!translation_voice_count_for_language("my-MM"));
    assert(translation_language_index("my-MM") == TRANSLATION_LANGUAGE_COUNT);
    assert(!translation_voice_count_for_language(NULL));
    assert(!translation_voice_count_for_language(""));
    assert(!translation_voice_count_for_language("eng-US"));
}

static void test_responses(void)
{
    char id[128];
    char state[32];
    assert(translation_parse_agent("{\"agent_id\":\"agent_abc-123\",\"status\":\"RUNNING\"}", id, sizeof(id)));
    assert(strcmp(id, "agent_abc-123") == 0);
    const char *invalid[] = {
        "{}", "null", "[]", "{", "{\"agent_id\":12}", "{\"agent_id\":\"\"}",
        "{\"agent_id\":\"../other\"}", "{\"agent_id\":\"abc?token=foo\"}",
        "{\"agent_id\":\"abc\"} trailing",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        assert(!translation_parse_agent(invalid[i], id, sizeof(id)));
    }
    assert(!translation_parse_agent("{\"agent_id\":\"longer\"}", id, 4));
    assert(translation_parse_status("{\"status\":\"STOPPED\"}", state, sizeof(state)));
    assert(strcmp(state, "STOPPED") == 0);
    assert(!translation_parse_status("{\"status\":false}", state, sizeof(state)));
}

static void test_channels(void)
{
    unsigned char entropy[16] = {0};
    char first[64], second[64];
    assert(!translation_channel_from_random(entropy, first, 40));
    assert(translation_channel_from_random(entropy, first, sizeof(first)));
    assert(strcmp(first, "mosaico_00000000000000000000000000000000") == 0);
    entropy[15] = 255;
    assert(translation_channel_from_random(entropy, second, sizeof(second)));
    assert(strcmp(first, second) != 0);
    assert(strlen(second) == 40);
}

int main(void)
{
    test_ring();
    test_audio_timeline();
    test_join_contract();
    test_voices();
    test_responses();
    test_channels();
    puts("PASS: PCM ring/timestamp expiry, saturated mixing, V2V contract, voice prefixes/language coverage, response validation, unique channels");
    return 0;
}

