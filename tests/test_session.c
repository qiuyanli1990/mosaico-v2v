/* Exercise the real session worker loop with deterministic network/RTC/NVS shims. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "session.c"

static jmp_buf task_exit;
static unsigned ticks_left, random_sequence, sequence;
static int64_t now_us;
static bool online, time_ready, portable, enabled[2], rtc_open[2], connected[2], rtc_failed[2];
static bool cancel_join, fail_leave, fail_journal, fail_query;
static int fail_join_source, fail_rtc_source;
static const char *query_status;
static unsigned joins[2], rtc_starts[2], rtc_closes[2], rtc_queries[2];
static unsigned connected_checks[2], failed_checks[2], agent_queries[2], leaves;
static char stored_agents[3][128], channels[2][64], sources[2][16], targets[2][16];
static char joined_voices[2][TRANSLATION_VOICE_ID_SIZE];
static app_snapshot_t state;

const char *esp_err_to_name(esp_err_t error) { (void)error; return "TEST_ERROR"; }
bool network_is_connected(void) { return online; }
bool network_time_ready(void) { return time_ready; }
void app_set_status(app_phase_t phase, const char *message)
{ state.phase = phase; snprintf(state.message, sizeof(state.message), "%s", message); }
void app_set_channel(const char *channel)
{ snprintf(state.channel, sizeof(state.channel), "%s", channel); }
void app_get_snapshot(app_snapshot_t *snapshot) { *snapshot = state; }
void usb_audio_get_stats(usb_audio_stats_t *stats) { memset(stats, 0, sizeof(*stats)); }
void media_set_enabled(media_source_t source, bool allow) { enabled[source] = allow; }
void media_set_portable_mode(bool value) { portable = value; }
void media_set_portable_input(media_source_t source) { (void)source; }
uint32_t media_rendered_frames(void) { return 0; }
void crash_trace_mark(crash_trace_slot_t slot, crash_trace_stage_t stage, uint32_t detail)
{ (void)slot; (void)stage; (void)detail; }
int64_t esp_timer_get_time(void) { return now_us; }
void esp_fill_random(void *bytes, size_t size) { memset(bytes, ++random_sequence, size); }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg,
                unsigned priority, TaskHandle_t *handle)
{
    assert(task == session_task && strcmp(name, "translation") == 0 && stack >= 12288);
    (void)arg; (void)priority; (void)handle;
    return pdPASS;
}
void vTaskDelay(unsigned ticks)
{
    now_us += ticks * 1000;
    if (--ticks_left == 0) longjmp(task_exit, 1);
}
static void run_ticks(unsigned count)
{
    ticks_left = count;
    if (setjmp(task_exit) == 0) session_task(NULL);
}

static unsigned key_index(const char *key)
{
    for (unsigned i = 0; i < 3; ++i) if (strcmp(key, agent_keys[i]) == 0) return i;
    assert(false);
    return 0;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ assert(strcmp(name, "translator") == 0 && mode == NVS_READWRITE); *handle = 1; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *capacity)
{
    (void)handle;
    const char *stored = stored_agents[key_index(key)];
    if (!*stored) return ESP_ERR_NVS_NOT_FOUND;
    assert(strlen(stored) < *capacity);
    strcpy(value, stored);
    *capacity = strlen(stored) + 1;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    (void)handle;
    if (fail_journal) return ESP_FAIL;
    snprintf(stored_agents[key_index(key)], sizeof(stored_agents[0]), "%s", value);
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{ (void)handle; stored_agents[key_index(key)][0] = '\0'; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

bool v2v_configured(void) { return true; }
esp_err_t v2v_join(const char *channel, const char *source_language, const char *uid,
                   const char *target_language, const char *voice, char *agent, size_t capacity)
{
    unsigned source = strcmp(uid, TRANSLATOR_SPEAKER_DEVICE_UID) == 0 ? 0 : 1;
    assert(strcmp(uid, source ? TRANSLATOR_MIC_DEVICE_UID : TRANSLATOR_SPEAKER_DEVICE_UID) == 0);
    assert(online && time_ready);
    assert(translation_language_tts_supported(target_language) ?
           voice && strncmp(voice, "fm_02_", 6) == 0 : voice == NULL);
    ++joins[source];
    if ((int)source == fail_join_source) return ESP_FAIL;
    snprintf(channels[source], sizeof(channels[source]), "%s", channel);
    snprintf(sources[source], sizeof(sources[source]), "%s", source_language);
    snprintf(targets[source], sizeof(targets[source]), "%s", target_language);
    snprintf(joined_voices[source], sizeof(joined_voices[source]), "%s", voice ? voice : "");
    snprintf(agent, capacity, "Agent%u", ++sequence);
    if (cancel_join) session_request(false);
    return ESP_OK;
}
esp_err_t v2v_leave(const char *agent)
{
    assert(online && *agent);
    if (fail_leave) return ESP_FAIL;
    ++leaves;
    return ESP_OK;
}
esp_err_t v2v_query(const char *agent, char *status, size_t capacity)
{
    assert(*agent);
    unsigned source = strcmp(agent, pending_agents[0]) == 0 ? 0 : 1;
    assert(strcmp(agent, pending_agents[source]) == 0);
    ++agent_queries[source];
    if (fail_query) return ESP_FAIL;
    snprintf(status, capacity, "%s", query_status);
    return ESP_OK;
}
esp_err_t translator_webrtc_start(media_source_t source, const char *channel, const char *uid)
{
    (void)uid;
    assert(!rtc_open[source] && !enabled[source] && strcmp(channel, channels[source]) == 0);
    ++rtc_starts[source];
    if ((int)source == fail_rtc_source) return ESP_FAIL;
    rtc_open[source] = true;
    return ESP_OK;
}
void translator_webrtc_stop(media_source_t source)
{
    assert(!enabled[source]);
    if (rtc_open[source]) ++rtc_closes[source];
    rtc_open[source] = false;
}
bool translator_webrtc_connected(media_source_t source)
{ ++connected_checks[source]; return connected[source]; }
bool translator_webrtc_failed(media_source_t source)
{ ++failed_checks[source]; return rtc_failed[source]; }
void translator_webrtc_query(media_source_t source)
{ assert(rtc_open[source]); ++rtc_queries[source]; }

static void reset(session_mode_t mode)
{
    atomic_store(&requested, false);
    atomic_store(&selected_mode, SESSION_MODE_LIVE_MEETING);
    atomic_store(&travel_turn, SESSION_TRAVEL_NONE);
    active = running = false;
    worker_count = 0;
    cleanup_retry_after_us = next_start_attempt_us = 0;
    now_us = 1000000;
    online = time_ready = true;
    cancel_join = fail_leave = fail_journal = fail_query = false;
    fail_join_source = fail_rtc_source = -1;
    query_status = "RUNNING";
    leaves = 0;
    memset(stored_agents, 0, sizeof(stored_agents));
    memset(pending_agents, 0, sizeof(pending_agents));
    memset(joins, 0, sizeof(joins));
    memset(rtc_starts, 0, sizeof(rtc_starts));
    memset(rtc_closes, 0, sizeof(rtc_closes));
    memset(rtc_queries, 0, sizeof(rtc_queries));
    memset(agent_queries, 0, sizeof(agent_queries));
    memset(connected_checks, 0, sizeof(connected_checks));
    memset(failed_checks, 0, sizeof(failed_checks));
    memset(rtc_open, 0, sizeof(rtc_open));
    memset(enabled, 0, sizeof(enabled));
    memset(rtc_failed, 0, sizeof(rtc_failed));
    memset(&state, 0, sizeof(state));
    connected[0] = connected[1] = true;
    session_set_voice("fm_02_male-qn-qingse");
    memset(joined_voices, 0, sizeof(joined_voices));
    session_set_mode(mode);
    session_set_languages(translation_language_index("zh-CN"), translation_language_index("en-US"));
}
static void start(void)
{
    assert(session_init() == ESP_OK);
    session_request(true);
    run_ticks(1);
}
static void stop(void)
{
    session_request(false);
    assert(!enabled[0] && !enabled[1]);
    run_ticks(1);
    assert(!active && !running && !worker_count && state.phase == APP_IDLE);
    assert(!rtc_open[0] && !rtc_open[1]);
}

static void test_media(void)
{
    reset(SESSION_MODE_MEDIA);
    connected[1] = false;
    rtc_failed[1] = true;
    start();
    assert(active && running && worker_count == 1 && !portable);
    assert(joins[0] == 1 && joins[1] == 0 && rtc_starts[0] == 1 && rtc_starts[1] == 0);
    assert(enabled[0] && !enabled[1]);
    assert(strcmp(sources[0], "zh-CN") == 0 && strcmp(targets[0], "en-US") == 0);
    assert(strstr(state.message, "media"));
    run_ticks(161);
    assert(agent_queries[0] && !agent_queries[1] && rtc_queries[0] && !rtc_queries[1]);
    assert(!connected_checks[1] && !failed_checks[1]);
    stop();
    assert(leaves == 1 && rtc_closes[0] == 1 && !rtc_closes[1]);
}
static void test_two_way(session_mode_t mode)
{
    reset(mode);
    connected[1] = false;
    start();
    assert(active && !running && !enabled[0] && !enabled[1]);
    assert(joins[0] == 1 && joins[1] == 1 && worker_count == 2);
    assert(strcmp(channels[0], channels[1]) != 0);
    assert(strcmp(sources[1], "en-US") == 0 && strcmp(targets[1], "zh-CN") == 0);
    connected[1] = true;
    run_ticks(1);
    assert(running && enabled[0] && enabled[1]);
    assert(portable == (mode == SESSION_MODE_PORTABLE));
    if (portable) {
        assert(strstr(channels[0], "_cn-en") && strstr(channels[1], "_en-cn"));
        assert(strstr(state.message, "Tap a language card"));
        session_set_travel_turn(SESSION_TRAVEL_SOURCE_LANGUAGE);
        run_ticks(1);
        assert(enabled[0] && enabled[1]);
    } else {
        assert(strstr(state.message, "live meeting"));
    }
    assert(agent_queries[0] && agent_queries[1]);
    stop();
    assert(leaves == 2 && rtc_closes[0] == 1 && rtc_closes[1] == 1);
}
static void test_mode_switches(void)
{
    reset(SESSION_MODE_MEDIA);
    start();
    stop();
    session_set_mode(SESSION_MODE_LIVE_MEETING);
    now_us += 3000000;
    session_request(true);
    run_ticks(1);
    assert(running && joins[0] == 2 && joins[1] == 1 && enabled[1]);
    stop();
    session_set_mode(SESSION_MODE_MEDIA);
    now_us += 3000000;
    session_request(true);
    run_ticks(1);
    assert(running && joins[0] == 3 && joins[1] == 1 && !enabled[1]);
    stop();
    assert(leaves == 4);
}
static void test_voice_selection(void)
{
    for (session_mode_t mode = 0; mode < SESSION_MODE_COUNT; ++mode) {
        reset(mode);
        const translation_voice_t *choice = translation_voice_for_language("en-US", 1);
        session_set_voice(choice->id + 6);
        assert(strcmp(selected_voice_id, choice->id) == 0);
        session_set_voice("fm_02_fm_02_invalid");
        assert(strcmp(selected_voice_id, choice->id) == 0);
        start();
        assert(strcmp(joined_voices[0], choice->id) == 0);
        if (mode != SESSION_MODE_MEDIA)
            assert(strcmp(joined_voices[1], translation_voice_for_language("zh-CN", 0)->id) == 0);
        stop();
    }
    reset(SESSION_MODE_LIVE_MEETING);
    session_set_languages(translation_language_index("en-US"), translation_language_index("ja-JP"));
    session_set_voice(translation_voice_for_language("ja-JP", 2)->id);
    start();
    assert(strcmp(joined_voices[0], translation_voice_for_language("ja-JP", 2)->id) == 0);
    assert(strcmp(joined_voices[1], translation_voice_for_language("en-US", 0)->id) == 0);
    stop();

    reset(SESSION_MODE_MEDIA);
    start(); /* The old Chinese default must not override the English target voice. */
    assert(strcmp(joined_voices[0], translation_voice_for_language("en-US", 0)->id) == 0);
    stop();

    reset(SESSION_MODE_LIVE_MEETING);
    session_set_languages(translation_language_index("en-US"), translation_language_index("tl-PH"));
    start();
    assert(!joined_voices[0][0]);
    assert(strcmp(joined_voices[1], translation_voice_for_language("en-US", 0)->id) == 0);
    stop();
}
static void test_failures(void)
{
    reset(SESSION_MODE_MEDIA);
    fail_rtc_source = 0;
    start();
    assert(state.phase == APP_ERROR && !session_requested() && !worker_count);
    assert(leaves == 1 && !joins[1] && !enabled[0]);
    reset(SESSION_MODE_MEDIA);
    fail_journal = true;
    start();
    assert(state.phase == APP_ERROR && leaves == 1 && !rtc_starts[0] && !joins[1]);
    reset(SESSION_MODE_LIVE_MEETING);
    fail_join_source = 1;
    start();
    assert(state.phase == APP_ERROR && leaves == 1 && rtc_closes[0] == 1);
    assert(!enabled[0] && !enabled[1] && !worker_count);
    reset(SESSION_MODE_LIVE_MEETING);
    cancel_join = true;
    start();
    assert(state.phase == APP_IDLE && !session_requested() && !joins[1] && leaves == 1);
    assert(!rtc_open[0] && !enabled[0]);
}
static void test_pending_cleanup(void)
{
    reset(SESSION_MODE_MEDIA);
    strcpy(stored_agents[0], "OldSpeaker");
    strcpy(stored_agents[1], "OldMic");
    strcpy(stored_agents[2], "LegacyAgent");
    fail_leave = true;
    start();
    assert(!joins[0] && !joins[1] && !session_requested());
    assert(pending_agents[0][0] && pending_agents[1][0] && pending_agents[2][0]);
    fail_leave = false;
    now_us += 3000000;
    session_request(true);
    run_ticks(1);
    assert(leaves == 3 && joins[0] == 1 && !joins[1] && running);
    assert(!stored_agents[1][0] && !stored_agents[2][0]);
    stop();
    assert(leaves == 4);
}
static void test_runtime_errors(void)
{
    reset(SESSION_MODE_MEDIA);
    start();
    fail_query = true;
    run_ticks(310);
    assert(agent_queries[0] == 3 && !agent_queries[1]);
    assert(state.phase == APP_ERROR && leaves == 1 && !session_requested());
    reset(SESSION_MODE_MEDIA);
    start();
    query_status = "STOPPED";
    run_ticks(1);
    assert(state.phase == APP_ERROR && leaves == 1 && !joins[1]);
    reset(SESSION_MODE_MEDIA);
    start();
    online = false;
    run_ticks(1);
    assert(state.phase == APP_ERROR && !rtc_open[0] && !session_requested());
    assert(pending_agents[0][0] && !pending_agents[1][0]);
    online = true;
    session_set_mode(SESSION_MODE_LIVE_MEETING);
    now_us += 3000000;
    session_request(true);
    run_ticks(1);
    assert(leaves == 1 && running && joins[0] == 2 && joins[1] == 1);
    stop();
}
int main(void)
{
    test_media();
    test_two_way(SESSION_MODE_LIVE_MEETING);
    test_two_way(SESSION_MODE_PORTABLE);
    test_mode_switches();
    test_voice_selection();
    test_failures();
    test_pending_cleanup();
    test_runtime_errors();
    puts("PASS: actual session loop: one-room media, two-way meeting/travel, language-matched voices in each direction, mode switches, cancellation, failures, journal and network/query cleanup");
    return 0;
}
