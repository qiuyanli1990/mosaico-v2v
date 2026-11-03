#include "translation_protocol.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static const translation_language_t languages[TRANSLATION_LANGUAGE_COUNT] = {
    {"af-ZA", "Afrikaans", "Afrikaans", true},
    {"sq-AL", "Albanian", "Shqip", false},
    {"ar-SA", "Arabic", "العربية", true},
    {"az-AZ", "Azerbaijani", "Azərbaycan", false},
    {"eu-ES", "Basque", "Euskara", false},
    {"be-BY", "Belarusian", "Беларуская", false},
    {"bn-IN", "Bengali", "বাংলা", false},
    {"bs-BA", "Bosnian", "Bosanski", false},
    {"bg-BG", "Bulgarian", "Български", true},
    {"ca-ES", "Catalan", "Català", true},
    {"zh-CN", "Chinese", "中文", true},
    {"hr-HR", "Croatian", "Hrvatski", true},
    {"cs-CZ", "Czech", "Čeština", true},
    {"da-DK", "Danish", "Dansk", true},
    {"nl-NL", "Dutch", "Nederlands", true},
    {"en-US", "English", "English", true},
    {"et-EE", "Estonian", "Eesti", false},
    {"fi-FI", "Finnish", "Suomi", true},
    {"fr-FR", "French", "Français", true},
    {"gl-ES", "Galician", "Galego", false},
    {"de-DE", "German", "Deutsch", true},
    {"el-GR", "Greek", "Ελληνικά", true},
    {"gu-IN", "Gujarati", "ગુજરાતી", false},
    {"he-IL", "Hebrew", "עברית", true},
    {"hi-IN", "Hindi", "हिन्दी", true},
    {"hu-HU", "Hungarian", "Magyar", true},
    {"id-ID", "Indonesian", "Bahasa Indonesia", true},
    {"it-IT", "Italian", "Italiano", true},
    {"ja-JP", "Japanese", "日本語", true},
    {"kn-IN", "Kannada", "ಕನ್ನಡ", false},
    {"kk-KZ", "Kazakh", "Қазақ", false},
    {"ko-KR", "Korean", "한국어", true},
    {"lv-LV", "Latvian", "Latviešu", false},
    {"lt-LT", "Lithuanian", "Lietuvių", false},
    {"mk-MK", "Macedonian", "Македонски", false},
    {"ms-MY", "Malay", "Bahasa Melayu", true},
    {"ml-IN", "Malayalam", "മലയാളം", false},
    {"mr-IN", "Marathi", "मराठी", false},
    {"no-NO", "Norwegian", "Norsk", true},
    {"fa-IR", "Persian", "فارسی", false},
    {"pl-PL", "Polish", "Polski", true},
    {"pt-BR", "Portuguese", "Português", true},
    {"pa-IN", "Punjabi", "ਪੰਜਾਬੀ", false},
    {"ro-RO", "Romanian", "Română", true},
    {"ru-RU", "Russian", "Русский", true},
    {"sr-RS", "Serbian", "Српски", false},
    {"sk-SK", "Slovak", "Slovenčina", true},
    {"sl-SI", "Slovenian", "Slovenščina", true},
    {"es-ES", "Spanish", "Español", true},
    {"sw-KE", "Swahili", "Kiswahili", false},
    {"sv-SE", "Swedish", "Svenska", true},
    {"tl-PH", "Filipino (Tagalog)", "Tagalog", false},
    {"ta-IN", "Tamil", "தமிழ்", false},
    {"te-IN", "Telugu", "తెలుగు", false},
    {"th-TH", "Thai", "ไทย", true},
    {"tr-TR", "Turkish", "Türkçe", true},
    {"uk-UA", "Ukrainian", "Українська", true},
    {"ur-PK", "Urdu", "اردو", false},
    {"vi-VN", "Vietnamese", "Tiếng Việt", true},
    {"cy-GB", "Welsh", "Cymraeg", false},
};

/* Official IDs and the native/multilingual selections are documented in docs/tts-voices.md. */
#define VOICE(id, name, languages) {"fm_02_" id, name, languages}
static const translation_voice_t voices[] = {
    VOICE("male-qn-qingse", "Qingse (male)", "zh"),
    VOICE("Chinese (Mandarin)_Warm_Bestie", "Warm Bestie (female)", "zh"),
    VOICE("Chinese (Mandarin)_Gentleman", "Gentleman (male)", "zh"),
    VOICE("Chinese (Mandarin)_News_Anchor", "News Anchor", "zh"),
    VOICE("Chinese (Mandarin)_Sweet_Lady", "Sweet Lady", "zh"),
    VOICE("English_Graceful_Lady", "Graceful Lady (female)", "en"),
    VOICE("English_Trustworth_Man", "Trustworthy Man", "en"),
    VOICE("English_expressive_narrator", "Expressive Narrator", "en"),
    VOICE("English_CalmWoman", "Calm Woman", "en"),
    VOICE("Japanese_GracefulMaiden", "Graceful Maiden", "ja"),
    VOICE("Japanese_GentleButler", "Gentle Butler (male)", "ja"),
    VOICE("Japanese_CalmLady", "Calm Lady", "ja"),
    VOICE("Korean_CalmLady", "Calm Lady", "ko"),
    VOICE("Korean_CalmGentleman", "Calm Gentleman", "ko"),
    VOICE("Korean_IntellectualMan", "Intellectual Man", "ko"),
    VOICE("German_FriendlyMan", "Friendly Man", "de"),
    VOICE("German_SweetLady", "Sweet Lady", "de"),
    VOICE("French_Male_Speech_New", "Level-Headed Man", "fr"),
    VOICE("French_Female_News Anchor", "Patient Presenter (female)", "fr"),
    VOICE("Spanish_SereneWoman", "Serene Woman", "es"),
    VOICE("Spanish_ThoughtfulMan", "Thoughtful Man", "es"),
    VOICE("Spanish_Narrator", "Narrator", "es"),
    VOICE("Portuguese_SereneWoman", "Serene Woman", "pt"),
    VOICE("Portuguese_ReliableMan", "Reliable Man", "pt"),
    VOICE("Russian_ReliableMan", "Reliable Man", "ru"),
    VOICE("Russian_AmbitiousWoman", "Ambitious Woman", "ru"),
    VOICE("Italian_Narrator", "Narrator", "it"),
    VOICE("Italian_BraveHeroine", "Brave Heroine", "it"),
    VOICE("Arabic_CalmWoman", "Calm Woman", "ar"),
    VOICE("Arabic_FriendlyGuy", "Friendly Guy", "ar"),
    VOICE("hindi_male_1_v2", "Trustworthy Advisor (male)", "hi"),
    VOICE("hindi_female_2_v1", "Tranquil Woman", "hi"),
    VOICE("Indonesian_CalmWoman", "Calm Woman", "id"),
    VOICE("Indonesian_CaringMan", "Caring Man", "id"),
    VOICE("Indonesian_GentleGirl", "Gentle Girl", "id"),
    VOICE("Vietnamese_kindhearted_girl", "Kind-Hearted Girl", "vi"),
    VOICE("Thai_female_1_sample1", "Confident Woman", "th"),
    VOICE("Thai_male_1_sample8", "Serene Man", "th"),
    VOICE("Thai_male_2_sample2", "Friendly Man", "th"),
    VOICE("Dutch_kindhearted_girl", "Kind-Hearted Girl", "nl"),
    VOICE("Dutch_bossy_leader", "Bossy Leader", "nl"),
    VOICE("Turkish_CalmWoman", "Calm Woman", "tr"),
    VOICE("Turkish_Trustworthyman", "Trustworthy Man", "tr"),
    VOICE("Ukrainian_CalmWoman", "Calm Woman", "uk"),
    VOICE("Ukrainian_WiseScholar", "Wise Scholar", "uk"),
    VOICE("Polish_female_1_sample1", "Calm Woman", "pl"),
    VOICE("Polish_male_1_sample4", "Male Narrator", "pl"),
    VOICE("Romanian_female_2_sample1", "Gentle Woman", "ro"),
    VOICE("Romanian_male_1_sample2", "Reliable Man", "ro"),
    VOICE("Greek_female_1_sample1", "Gentle Lady", "el"),
    VOICE("greek_male_1a_v1", "Thoughtful Mentor", "el"),
    VOICE("czech_male_1_v1", "Assured Presenter", "cs"),
    VOICE("czech_female_2_v2", "Elegant Lady", "cs"),
    VOICE("finnish_male_3_v1", "Upbeat Man", "fi"),
    VOICE("finnish_female_4_v1", "Assertive Woman", "fi"),
    /* These supported languages have no native IDs in MiniMax's published list. */
    VOICE("English_Graceful_Lady", "Graceful Lady (multilingual)", "af bg ca hr da he hu ms no sk sl sv"),
    VOICE("English_Trustworth_Man", "Trustworthy Man (multilingual)", "af bg ca hr da he hu ms no sk sl sv"),
};
#undef VOICE

static bool voice_matches_language(const translation_voice_t *voice, const char *language_code)
{
    if (!language_code || strlen(language_code) < 2) return false;
    size_t code_length = strcspn(language_code, "-_");
    const char *languages = voice->language;
    while (*languages) {
        size_t length = strcspn(languages, " ");
        if (length == code_length && strncmp(languages, language_code, length) == 0) return true;
        languages += length;
        while (*languages == ' ') ++languages;
    }
    return false;
}

size_t translation_voice_count_for_language(const char *language_code)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(voices) / sizeof(voices[0]); ++i) {
        count += voice_matches_language(&voices[i], language_code) ? 1 : 0;
    }
    return count;
}

const translation_voice_t *translation_voice_for_language(const char *language_code,
                                                          size_t option_index)
{
    for (size_t i = 0, matched = 0; i < sizeof(voices) / sizeof(voices[0]); ++i) {
        if (voice_matches_language(&voices[i], language_code) && matched++ == option_index) {
            return &voices[i];
        }
    }
    return NULL;
}

bool translation_format_voice_id(const char *id, char *out, size_t capacity)
{
    if (!id || !*id || !out || !capacity) return false;
    const char *raw = strncmp(id, "fm_02_", 6) == 0 ? id + 6 : id;
    if (!*raw || strncmp(raw, "fm_02_", 6) == 0 || strlen(raw) + 7 > capacity) return false;
    memmove(out + 6, raw, strlen(raw) + 1);
    memcpy(out, "fm_02_", 6);
    return true;
}

const translation_language_t *translation_language_at(size_t index)
{
    return index < TRANSLATION_LANGUAGE_COUNT ? &languages[index] : NULL;
}

size_t translation_language_index(const char *code)
{
    if (!code) {
        return TRANSLATION_LANGUAGE_COUNT;
    }
    for (size_t i = 0; i < TRANSLATION_LANGUAGE_COUNT; ++i) {
        if (strcmp(languages[i].code, code) == 0) {
            return i;
        }
    }
    return TRANSLATION_LANGUAGE_COUNT;
}

size_t translation_tts_language_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < TRANSLATION_LANGUAGE_COUNT; ++i) {
        count += languages[i].tts_supported ? 1 : 0;
    }
    return count;
}

bool translation_language_tts_supported(const char *code)
{
    size_t index = translation_language_index(code);
    return index < TRANSLATION_LANGUAGE_COUNT && languages[index].tts_supported;
}

bool translation_channel_from_random(const unsigned char random[16], char *channel, size_t capacity)
{
    static const char hex[] = "0123456789abcdef";
    const size_t prefix = sizeof("mosaico_") - 1;
    if (!random || !channel || capacity < prefix + 32 + 1) {
        return false;
    }
    memcpy(channel, "mosaico_", prefix);
    for (size_t i = 0; i < 16; ++i) {
        channel[prefix + i * 2] = hex[random[i] >> 4];
        channel[prefix + i * 2 + 1] = hex[random[i] & 15];
    }
    channel[prefix + 32] = '\0';
    return true;
}

bool translation_valid_agent_id(const char *id)
{
    if (!id || !*id || strlen(id) >= 128) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) {
            return false;
        }
    }
    return true;
}

static bool numeric_uid(const char *uid)
{
    if (!uid || !*uid || strlen(uid) > 10) {
        return false;
    }
    unsigned long long value = 0;
    for (const unsigned char *p = (const unsigned char *)uid; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + *p - '0';
    }
    return value > 0 && value <= 0xffffffffULL;
}

char *translation_join_json(const translation_request_t *request)
{
    char voice_id[TRANSLATION_VOICE_ID_SIZE];
    if (!request || !request->channel || !*request->channel ||
        strlen(request->channel) >= 64 || !request->source_language ||
        !request->target_language || translation_language_index(request->source_language) >= TRANSLATION_LANGUAGE_COUNT ||
        translation_language_index(request->target_language) >= TRANSLATION_LANGUAGE_COUNT ||
        (request->tts_enabled && !translation_format_voice_id(request->voice_id, voice_id, sizeof(voice_id))) ||
        (request->tts_enabled && !translation_language_tts_supported(request->target_language)) ||
        !numeric_uid(request->device_uid) || !numeric_uid(request->agent_uid) ||
        strcmp(request->device_uid, request->agent_uid) == 0 || !request->idle_timeout) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *properties = cJSON_AddObjectToObject(root, "properties");
    cJSON *remote = properties ? cJSON_AddArrayToObject(properties, "remote_rtc_uids") : NULL;
    cJSON *features = properties ? cJSON_AddObjectToObject(properties, "advanced_features") : NULL;
    cJSON *parameters = properties ? cJSON_AddObjectToObject(properties, "parameters") : NULL;
    cJSON *asr = properties ? cJSON_AddObjectToObject(properties, "asr") : NULL;
    cJSON *translation = properties ? cJSON_AddObjectToObject(properties, "translation") : NULL;
    cJSON *tts = properties ? cJSON_AddObjectToObject(properties, "tts") : NULL;
    cJSON *remote_uid = cJSON_CreateString(request->device_uid);
    if (!remote || !features || !parameters || !asr || !translation || !tts || !remote_uid) {
        cJSON_Delete(remote_uid);
        cJSON_Delete(root);
        return NULL;
    }
    if (!cJSON_AddItemToArray(remote, remote_uid)) {
        cJSON_Delete(remote_uid);
        cJSON_Delete(root);
        return NULL;
    }
    bool ok = cJSON_AddStringToObject(root, "name", request->channel) &&
        cJSON_AddStringToObject(root, "preset", "v2vt_base") &&
        cJSON_AddStringToObject(properties, "channel", request->channel) &&
        cJSON_AddStringToObject(properties, "agent_rtc_uid", request->agent_uid) &&
        cJSON_AddBoolToObject(properties, "enable_string_uid", true) &&
        cJSON_AddNumberToObject(properties, "idle_timeout", request->idle_timeout) &&
        cJSON_AddBoolToObject(features, "enable_rtm", false) &&
        cJSON_AddStringToObject(parameters, "data_channel", "datastream") &&
        cJSON_AddStringToObject(asr, "language", request->source_language) &&
        cJSON_AddStringToObject(translation, "language", request->target_language) &&
        cJSON_AddBoolToObject(tts, "enable", request->tts_enabled) &&
        (!request->tts_enabled ||
         (cJSON_AddStringToObject(tts, "voice_id", voice_id) &&
          cJSON_AddNumberToObject(tts, "speed", request->tts_speed)));
    /* Certificate-disabled project: deliberately omit the RTC token field. */
    char *serialized = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return serialized;
}

static bool parse_string(const char *json, const char *key, char *out, size_t capacity)
{
    if (!json || !out || !capacity) {
        return false;
    }
    out[0] = '\0';
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &end, true);
    cJSON *field = root ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
    bool ok = cJSON_IsObject(root) && cJSON_IsString(field) && *field->valuestring &&
              strlen(field->valuestring) < capacity;
    if (ok) {
        memcpy(out, field->valuestring, strlen(field->valuestring) + 1);
    }
    cJSON_Delete(root);
    return ok;
}

bool translation_parse_agent(const char *json, char *agent_id, size_t capacity)
{
    if (!parse_string(json, "agent_id", agent_id, capacity)) {
        return false;
    }
    if (!translation_valid_agent_id(agent_id)) {
        agent_id[0] = '\0';
        return false;
    }
    return true;
}

bool translation_parse_status(const char *json, char *status, size_t capacity)
{
    return parse_string(json, "status", status, capacity);
}

