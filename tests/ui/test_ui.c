#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../main/ui.c"

static network_status_t mock_network;
static app_snapshot_t mock_state = {.phase = APP_IDLE};
static usb_audio_stats_t mock_usb;
static ui_battery_t mock_battery;
static session_mode_t mock_mode;
static session_travel_turn_t mock_turn;
static bool mock_requested, mock_time_ready, brightness_failure;
static unsigned starts, stops, scans, connections_count, brightness_writes;
static unsigned mock_volume = 70, last_brightness, source_index, target_index;
static size_t mock_ap_count = 2;
static uint32_t subtitle_generation;
static void *page_pool;
static lv_display_t *display;
static unsigned flushes;
static uint16_t pixels[480 * 480], draw_buffer[480 * 40];
static const char *output;
static char mock_voice[TRANSLATION_VOICE_ID_SIZE];

void *heap_caps_malloc(size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    page_pool = malloc(size);
    return page_pool;
}
void heap_caps_free(void *memory) { free(memory); }
void *bsp_display_start(void) { return display; }
bool bsp_display_lock(int timeout) { (void)timeout; return true; }
void bsp_display_unlock(void) {}
esp_err_t bsp_display_brightness_set(int value)
{
    ++brightness_writes;
    assert(value >= 10 && value <= 100);
    if (brightness_failure) return ESP_FAIL;
    last_brightness = (unsigned)value;
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t value) { (void)value; return "TEST_ERROR"; }
void ui_device_init(void) {}
void ui_device_get_battery(ui_battery_t *battery) { *battery = mock_battery; }
void network_get_status(network_status_t *status) { *status = mock_network; }
bool network_is_connected(void) { return mock_network.connection == NETWORK_CONNECTED; }
bool network_time_ready(void) { return mock_time_ready; }
esp_err_t network_scan_start(void) { ++scans; return ESP_OK; }
size_t network_get_scan_results(network_ap_t *aps, size_t capacity)
{
    assert(capacity >= mock_ap_count);
    aps[0] = (network_ap_t){.ssid = "Coffee & Clouds", .rssi = -42, .secure = true};
    aps[1] = (network_ap_t){.ssid = "Open garden", .rssi = -60};
    for (size_t i = 2; i < mock_ap_count; ++i) {
        aps[i] = (network_ap_t){.rssi = -70, .secure = true};
        snprintf(aps[i].ssid, sizeof(aps[i].ssid), "Garden %02u", (unsigned)i);
    }
    return mock_ap_count;
}
esp_err_t network_connect(const char *ssid, const char *password)
{
    assert(strcmp(ssid, "Coffee & Clouds") == 0 || strcmp(ssid, "Open garden") == 0);
    assert(strcmp(password, strcmp(ssid, "Coffee & Clouds") == 0 ? "testpass123" : "") == 0);
    ++connections_count;
    return ESP_OK;
}
void app_get_snapshot(app_snapshot_t *snapshot) { *snapshot = mock_state; }
bool session_requested(void) { return mock_requested; }
void session_request(bool start)
{
    if (start) ++starts;
    else {
        ++stops;
        mock_turn = SESSION_TRAVEL_NONE;
        if (mock_requested || mock_state.phase == APP_RUNNING) mock_state.phase = APP_STOPPING;
    }
    mock_requested = start;
}
void session_set_mode(session_mode_t mode) { mock_mode = mode; mock_turn = SESSION_TRAVEL_NONE; }
session_mode_t session_get_mode(void) { return mock_mode; }
void session_set_languages(size_t source, size_t target) { source_index = source; target_index = target; }
void session_set_voice(const char *voice)
{
    assert(!voice || strncmp(voice, "fm_02_", 6) == 0);
    snprintf(mock_voice, sizeof(mock_voice), "%s", voice ? voice : "");
}
void session_set_travel_turn(session_travel_turn_t turn) { mock_turn = turn; }
session_travel_turn_t session_get_travel_turn(void) { return mock_turn; }
void media_set_volume(unsigned volume) { mock_volume = volume; }
unsigned media_get_volume(void) { return mock_volume; }
unsigned media_get_portable_input_level(void) { return 34; }
void usb_audio_get_stats(usb_audio_stats_t *stats) { *stats = mock_usb; }
void subtitles_clear(void) { ++subtitle_generation; }
uint32_t subtitles_generation(void) { return subtitle_generation; }
bool subtitles_copy_channel(unsigned channel, char *text, size_t capacity)
{
    if (mock_state.phase != APP_RUNNING) return false;
    snprintf(text, capacity, "%s", channel ? "Nice to meet you, too!" : "Hello! So lovely to meet you.");
    return true;
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *data)
{
    ++flushes;
    uint16_t *source = (uint16_t *)data;
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(pixels + y * 480 + area->x1, source, (size_t)(area->x2 - area->x1 + 1) * sizeof(uint16_t));
        source += area->x2 - area->x1 + 1;
    }
    lv_display_flush_ready(disp);
}

static void check_page(page_id_t expected)
{
    assert(current_page == expected);
    for (unsigned i = 0; i < PAGE_COUNT; ++i)
        assert(lv_obj_has_flag(pages[i], LV_OBJ_FLAG_HIDDEN) == (i != (unsigned)expected));
}

static void click(lv_obj_t *object) { lv_obj_send_event(object, LV_EVENT_CLICKED, NULL); }
static void back(void) { click(return_buttons[current_page]); }

static void screenshot(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", output, name);
    lv_refr_now(display);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n480 480\n255\n");
    for (size_t i = 0; i < 480 * 480; ++i) {
        unsigned char rgb[] = {((pixels[i] >> 11) & 31) * 255 / 31,
                               ((pixels[i] >> 5) & 63) * 255 / 63,
                               (pixels[i] & 31) * 255 / 31};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}

static void change(lv_obj_t *object, int value)
{
    lv_slider_set_value(object, value, LV_ANIM_OFF);
    lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, NULL);
}

static void choose_option(lv_obj_t *dropdown, unsigned index)
{
    lv_dropdown_set_selected(dropdown, index);
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, NULL);
}

static void assert_inside(lv_obj_t *object, int left, int top, int right, int bottom)
{
    lv_obj_update_layout(lv_screen_active());
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= left && area.y1 >= top && area.x2 <= right && area.y2 <= bottom);
}

static void assert_language_flow(unsigned index, const char *source, const char *target, bool media)
{
    direction_t *direction = &directions[index];
    char expected[128];
    snprintf(expected, sizeof(expected), media ? "%s  " LV_SYMBOL_RIGHT "  %s" :
             "%s\n" LV_SYMBOL_DOWN "\n%s", source, target);
    assert(strcmp(lv_label_get_text(direction->flow), expected) == 0);
    const lv_font_t *font = lv_obj_get_style_text_font(direction->flow, 0);
    lv_point_t size;
    lv_text_get_size(&size, expected, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    assert(size.x <= lv_obj_get_width(direction->flow));
    assert(size.y <= lv_obj_get_height(direction->flow));
    lv_area_t root, flow, hint, state;
    lv_obj_get_coords(direction->root, &root);
    lv_obj_get_coords(direction->flow, &flow);
    lv_obj_get_coords(direction->state, &state);
    assert(flow.x1 >= root.x1 + 12 && flow.x2 <= root.x2 - 12);
    assert(flow.y1 >= root.y1 + 12 && flow.y2 + 4 < state.y1);
    if (!lv_obj_has_flag(direction->hint, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_get_coords(direction->hint, &hint);
        assert(flow.y2 + 4 < hint.y1 && hint.y2 < state.y1);
    }
}

static void test_full_language_names(void)
{
    size_t long_name = translation_language_index("tl-PH");
    size_t other = translation_language_index("az-AZ");
    const char *previews[] = {"12-meeting-long-languages", "13-travel-long-languages", "14-media-long-languages"};
    for (session_mode_t mode = 0; mode < SESSION_MODE_COUNT; ++mode) {
        session_set_mode(mode);
        setup_t *setup = &setups[mode];
        show_page(PAGE_TRANSLATE);
        for (size_t i = 0; i < TRANSLATION_LANGUAGE_COUNT; ++i) {
            size_t target = i == long_name ? other : long_name;
            lv_dropdown_set_selected(setup->source, i);
            lv_dropdown_set_selected(setup->target, target);
            configure_directions();
            lv_obj_update_layout(lv_screen_active());
            const translation_language_t *from = translation_language_at(i);
            const translation_language_t *to = translation_language_at(target);
            assert_language_flow(0, from->english_name, to->english_name, mode == SESSION_MODE_MEDIA);
            if (mode != SESSION_MODE_MEDIA)
                assert_language_flow(1, to->english_name, from->english_name, false);
        }
        lv_dropdown_set_selected(setup->source, long_name);
        lv_dropdown_set_selected(setup->target, other);
        configure_directions();
        refresh(NULL);
        screenshot(previews[mode]);
        lv_dropdown_set_selected(setup->source, translation_language_index("zh-CN"));
        lv_dropdown_set_selected(setup->target, translation_language_index("en-US"));
    }
    session_set_mode(SESSION_MODE_LIVE_MEETING);
    show_page(PAGE_HOME);
    refresh(NULL);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    output = argv[1];
    lv_init();
    display = lv_display_create(480, 480);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    esp_err_t init_result = ui_init();
    if (init_result != ESP_OK) fprintf(stderr, "UI init failed: %d, display=%p pool=%p\n", init_result, (void *)display, page_pool);
    assert(init_result == ESP_OK);
    check_page(PAGE_HOME);
    assert(scans == 0 && starts == 0);
    assert(strcmp(lv_label_get_text(home_clock), "--:--") == 0);
    assert(strstr(lv_label_get_text(home_battery), "--%"));
    screenshot("01-home-offline");
    mock_network.connection = NETWORK_CONNECTED;
    strcpy(mock_network.ssid, "Coffee & Clouds");
    mock_time_ready = true;
    mock_battery = (ui_battery_t){.available = true, .charging = true, .percent = 86};
    refresh(NULL);
    assert(strcmp(lv_label_get_text(home_clock), "--:--") != 0);
    assert(strstr(lv_label_get_text(home_battery), "86%"));
    screenshot("02-home");
    brightness_writes = 0;
    change(brightness_sliders[0], 36);
    assert(brightness_writes == 0 && lv_slider_get_value(brightness_sliders[1]) == 36);
    lv_obj_send_event(brightness_sliders[0], LV_EVENT_RELEASED, NULL);
    assert(brightness_writes == 1 && last_brightness == 36);
    brightness_failure = true;
    change(brightness_sliders[0], 60);
    lv_obj_send_event(brightness_sliders[0], LV_EVENT_RELEASED, NULL);
    assert(brightness == 36 && lv_slider_get_value(brightness_sliders[0]) == 36);
    brightness_failure = false;
    open_modes(NULL);
    check_page(PAGE_MODES);
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_y(mode_buttons[SESSION_MODE_MEDIA]) < lv_obj_get_y(mode_buttons[SESSION_MODE_LIVE_MEETING]));
    assert(lv_obj_get_y(mode_buttons[SESSION_MODE_LIVE_MEETING]) < lv_obj_get_y(mode_buttons[SESSION_MODE_PORTABLE]));
    screenshot("03-modes");
    click(mode_buttons[0]);
    check_page(PAGE_MEETING_SETUP);
    assert(starts == 0 && source_index == translation_language_index("zh-CN"));
    assert(target_index == translation_language_index("en-US"));
    assert(lv_dropdown_get_option_count(setups[0].voice) == translation_voice_count_for_language("en-US"));
    assert(strcmp(mock_voice, translation_voice_for_language("en-US", 0)->id) == 0);
    choose_option(setups[0].voice, 1);
    assert(strcmp(mock_voice, translation_voice_for_language("en-US", 1)->id) == 0);
    choose_option(setups[0].target, translation_language_index("ja-JP"));
    assert(lv_dropdown_get_option_count(setups[0].voice) == translation_voice_count_for_language("ja-JP"));
    assert(strcmp(mock_voice, translation_voice_for_language("ja-JP", 0)->id) == 0);
    choose_option(setups[0].voice, 2);
    assert(strcmp(mock_voice, translation_voice_for_language("ja-JP", 2)->id) == 0);
    choose_option(setups[0].target, translation_language_index("ms-MY"));
    assert(strstr(lv_dropdown_get_options(setups[0].voice), "multilingual"));
    choose_option(setups[0].target, translation_language_index("tl-PH"));
    assert(!mock_voice[0]);
    assert(strcmp(lv_dropdown_get_options(setups[0].voice), "No voices available") == 0);
    choose_option(setups[0].target, translation_language_index("en-US"));
    assert(lv_obj_get_parent(volume_slider) != pages[PAGE_MEETING_SETUP]);
    screenshot("04-meeting-setup");
    lv_dropdown_open(setups[0].source);
    assert(lv_dropdown_is_open(setups[0].source));
    back();
    check_page(PAGE_MODES);
    assert(!lv_dropdown_is_open(setups[0].source));
    click(mode_buttons[0]);
    lv_dropdown_open(setups[0].target);
    ui_show_wifi_settings();
    check_page(PAGE_WIFI);
    assert(!lv_dropdown_is_open(setups[0].target));
    back();
    check_page(PAGE_MEETING_SETUP);
    lv_dropdown_set_selected(setups[0].source, target_index);
    lv_obj_send_event(setups[0].source, LV_EVENT_VALUE_CHANGED, NULL);
    assert(lv_obj_has_state(setups[0].begin, LV_STATE_DISABLED));
    click(setups[0].begin);
    assert(starts == 0);
    check_page(PAGE_MEETING_SETUP);
    lv_dropdown_set_selected(setups[0].source, translation_language_index("zh-CN"));
    lv_obj_send_event(setups[0].source, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!lv_obj_has_state(setups[0].begin, LV_STATE_DISABLED));
    click(setups[0].begin);
    check_page(PAGE_TRANSLATE);
    assert(mock_requested && starts == 1);
    mock_state.phase = APP_RUNNING;
    strcpy(mock_state.message, "Two-way translation for your live meeting.");
    mock_usb.mounted = true;
    mock_usb.level = 52;
    ++subtitle_generation;
    refresh(NULL);
    assert(strcmp(lv_label_get_text(translation_title), "Live meeting") == 0);
    assert(strcmp(lv_label_get_text(directions[0].state), "MEETING TO YOU") == 0);
    assert(strcmp(lv_label_get_text(directions[1].state), "YOU TO MEETING") == 0);
    assert(strcmp(lv_label_get_text(directions[0].hint), "Computer speaker:\nChoose mosaico") == 0);
    assert(strcmp(lv_label_get_text(directions[1].hint), "Computer mic:\nChoose mosaico") == 0);
    click(directions[0].root);
    assert(mock_turn == SESSION_TRAVEL_NONE);
    change(volume_slider, 42);
    assert(mock_volume == 42);
    change(brightness_sliders[1], 65);
    lv_obj_send_event(brightness_sliders[1], LV_EVENT_RELEASED, NULL);
    assert(last_brightness == 65 && lv_slider_get_value(brightness_sliders[0]) == 65);
    screenshot("05-meeting-translation");
    unsigned before_flushes = flushes;
    refresh(NULL);
    lv_refr_now(display);
    assert(flushes == before_flushes);
    assert_inside(volume_slider, 28, 348, 452, 458);
    assert_inside(brightness_sliders[1], 28, 348, 452, 458);

    unsigned before_stops = stops;
    ui_show_wifi_settings();
    check_page(PAGE_WIFI);
    assert(mock_requested && stops == before_stops);
    ++mock_network.scan_generation;
    refresh(NULL);
    screenshot("06-wifi");
    click(lv_obj_get_child(ap_list, 0));
    check_page(PAGE_PASSWORD);
    screenshot("07-password");
    lv_textarea_set_text(password_input, "short");
    connect_selected();
    assert(connections_count == 0);
    lv_textarea_set_text(password_input, "discard-me");
    back();
    check_page(PAGE_WIFI);
    assert(lv_textarea_get_text(password_input)[0] == '\0');
    click(lv_obj_get_child(ap_list, 0));
    lv_textarea_set_text(password_input, "testpass123");
    connect_selected();
    check_page(PAGE_WIFI);
    assert(connections_count == 1 && lv_textarea_get_text(password_input)[0] == '\0');
    click(lv_obj_get_child(ap_list, 1));
    check_page(PAGE_WIFI);
    assert(connections_count == 2 && !selected_ap.secure);
    back();
    check_page(PAGE_TRANSLATE);
    assert(mock_requested && stops == before_stops);
    back();
    check_page(PAGE_MEETING_SETUP);
    assert(!mock_requested && stops == before_stops + 1);
    assert(lv_obj_has_state(setups[0].begin, LV_STATE_DISABLED));
    back();
    check_page(PAGE_MODES);
    click(mode_buttons[1]);
    check_page(PAGE_MODES);
    mock_state.phase = APP_IDLE;
    refresh(NULL);
    click(mode_buttons[1]);
    check_page(PAGE_TRAVEL_SETUP);
    screenshot("08-travel-setup");
    click(setups[1].begin);
    check_page(PAGE_TRANSLATE);
    assert(mock_turn == SESSION_TRAVEL_NONE && starts == 2);
    for (unsigned i = 0; i < 2; ++i) {
        assert(lv_obj_has_flag(directions[i].hint, LV_OBJ_FLAG_HIDDEN));
        assert(lv_label_get_text(directions[i].hint)[0] == '\0');
    }
    mock_state.phase = APP_RUNNING;
    strcpy(mock_state.message, "Tap a language card and speak.");
    ++subtitle_generation;
    refresh(NULL);
    assert(lv_label_get_text(directions[0].hint)[0] == '\0');
    click(directions[0].root);
    assert(mock_turn == SESSION_TRAVEL_SOURCE_LANGUAGE);
    screenshot("09-travel-translation");
    click(directions[0].root);
    assert(mock_turn == SESSION_TRAVEL_NONE);
    click(directions[1].root);
    assert(mock_turn == SESSION_TRAVEL_TARGET_LANGUAGE);
    click(translation_action);
    assert(!mock_requested && mock_turn == SESSION_TRAVEL_NONE);
    unsigned before_starts = starts;
    click(directions[0].root);
    assert(starts == before_starts);
    mock_state.phase = APP_IDLE;
    refresh(NULL);
    click(directions[0].root);
    assert(mock_requested && starts == before_starts + 1);
    back();
    check_page(PAGE_TRAVEL_SETUP);
    mock_state.phase = APP_IDLE;
    refresh(NULL);
    back();
    check_page(PAGE_MODES);
    click(mode_buttons[SESSION_MODE_MEDIA]);
    check_page(PAGE_MEDIA_SETUP);
    assert(mock_mode == SESSION_MODE_MEDIA);
    assert(source_index == translation_language_index("zh-CN") && target_index == translation_language_index("en-US"));
    screenshot("10-media-setup");
    before_starts = starts;
    click(setups[SESSION_MODE_MEDIA].begin);
    check_page(PAGE_TRANSLATE);
    assert(starts == before_starts + 1 && mock_turn == SESSION_TRAVEL_NONE);
    assert(strcmp(lv_label_get_text(translation_title), "Media translation") == 0);
    assert(lv_obj_has_flag(directions[1].root, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(directions[0].hint, LV_OBJ_FLAG_HIDDEN));
    const char *media_hint = "Plug Mosaico into your Computer and select mosaico as the speaker, or connect it to your phone.";
    assert(strcmp(lv_label_get_text(directions[0].hint), media_hint) == 0);
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_width(directions[0].root) == 424);
    assert_inside(directions[0].hint, 28, 153, 452, 288);
    assert(lv_obj_get_y(directions[0].hint) + lv_obj_get_height(directions[0].hint) <=
           lv_obj_get_y(directions[0].state));
    mock_state.phase = APP_RUNNING;
    strcpy(mock_state.message, "Translating media from your computer or phone.");
    ++subtitle_generation;
    refresh(NULL);
    assert(strcmp(lv_label_get_text(directions[0].hint), media_hint) == 0);
    click(directions[0].root);
    assert(mock_turn == SESSION_TRAVEL_NONE);
    assert(strcmp(lv_label_get_text(directions[0].state), "MEDIA TO YOU") == 0);
    screenshot("11-media-translation");
    ui_show_wifi_settings();
    check_page(PAGE_WIFI);
    back();
    check_page(PAGE_TRANSLATE);
    assert(mock_requested);
    back();
    check_page(PAGE_MEDIA_SETUP);
    assert(!mock_requested && lv_obj_has_state(setups[SESSION_MODE_MEDIA].begin, LV_STATE_DISABLED));
    back();
    check_page(PAGE_MODES);
    click(mode_buttons[SESSION_MODE_LIVE_MEETING]);
    check_page(PAGE_MODES);
    mock_state.phase = APP_IDLE;
    refresh(NULL);
    click(mode_buttons[SESSION_MODE_LIVE_MEETING]);
    check_page(PAGE_MEETING_SETUP);
    click(setups[SESSION_MODE_LIVE_MEETING].begin);
    check_page(PAGE_TRANSLATE);
    assert(!lv_obj_has_flag(directions[1].root, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(directions[1].hint, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(directions[0].hint), "Computer speaker:\nChoose mosaico") == 0);
    assert(strcmp(lv_label_get_text(directions[1].hint), "Computer mic:\nChoose mosaico") == 0);
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_width(directions[0].root) == 204);
    back();
    check_page(PAGE_MEETING_SETUP);
    mock_state.phase = APP_IDLE;
    refresh(NULL);
    back();
    check_page(PAGE_MODES);
    back();
    check_page(PAGE_HOME);
    ui_show_wifi_settings();
    mock_ap_count = NETWORK_MAX_APS;
    for (unsigned i = 0; i < 3; ++i) {
        ++mock_network.scan_generation;
        refresh(NULL);
        assert(lv_obj_get_child_count(ap_list) == NETWORK_MAX_APS);
        lv_refr_now(display);
    }
    click(lv_obj_get_child(ap_list, 0));
    check_page(PAGE_PASSWORD);
    lv_refr_now(display);
    back();
    check_page(PAGE_WIFI);
    back();
    check_page(PAGE_HOME);
    open_modes(NULL);
    click(mode_buttons[SESSION_MODE_LIVE_MEETING]);
    check_page(PAGE_MEETING_SETUP);
    lv_dropdown_open(setups[SESSION_MODE_LIVE_MEETING].target);
    lv_refr_now(display);
    lv_mem_monitor_t menu_memory;
    lv_mem_monitor(&menu_memory);
    assert(menu_memory.free_size > 32 * 1024);
    back();
    check_page(PAGE_MODES);
    back();
    check_page(PAGE_HOME);
    test_full_language_names();
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    printf("PASS: three-mode navigation, all 60 full language names fit each mode without clipping/overlap, meeting copy, single media card, travel without subtitles, dropdown dismissal, language guard, cleanup gating, Wi-Fi return/password/open AP/full scans, brightness sync/errors, volume; LVGL used=%zu free=%zu\n",
           memory.total_size - memory.free_size, memory.free_size);
    lv_deinit();
    free(page_pool);
    return 0;
}
