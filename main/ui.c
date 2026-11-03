#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_state.h"
#include "bsp/esp_mosaico.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "media.h"
#include "network.h"
#include "session.h"
#include "subtitles.h"
#include "translation_protocol.h"
#include "ui_device.h"
#include "usb_audio.h"
#include "usb_descriptors.h"

#define CREAM 0xFFF9EF
#define INK 0x394E46
#define MUTED 0x79867C
#define GREEN 0x47755F
#define MINT 0xDEEDDB
#define PEACH 0xF7DDCF
#define PINK 0xF3DCE0
#define CREAM_PINK 0xF0CBD3
#define ROSE 0xC98D9A
#define BUTTER 0xF4D78D
#define PALE_YELLOW 0xF7E8BE
#define WHITE 0xFFFFFF
#define LINE 0xE8E6DA

typedef enum {
    PAGE_HOME, PAGE_MODES, PAGE_MEETING_SETUP, PAGE_TRAVEL_SETUP, PAGE_MEDIA_SETUP,
    PAGE_TRANSLATE, PAGE_WIFI, PAGE_PASSWORD, PAGE_COUNT,
} page_id_t;

typedef struct {
    lv_obj_t *source;
    lv_obj_t *target;
    lv_obj_t *voice;
    lv_obj_t *hint;
    lv_obj_t *begin;
    char voices[512];
} setup_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *flow;
    lv_obj_t *hint;
    lv_obj_t *state;
    lv_obj_t *meter;
    unsigned background;
} direction_t;

static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *wifi_captions[PAGE_COUNT];
static lv_obj_t *return_buttons[PAGE_COUNT];
static page_id_t current_page = PAGE_HOME;
static page_id_t wifi_origin = PAGE_HOME;
static const page_id_t setup_pages[SESSION_MODE_COUNT] = {
    PAGE_MEETING_SETUP, PAGE_TRAVEL_SETUP, PAGE_MEDIA_SETUP,
};
static setup_t setups[SESSION_MODE_COUNT];
static direction_t directions[2];
static lv_obj_t *mode_buttons[SESSION_MODE_COUNT], *mode_status;
static lv_obj_t *home_clock, *home_date, *home_battery, *home_battery_note;
static lv_obj_t *translation_title, *translation_status, *translation_action, *translation_action_text;
static lv_obj_t *volume_slider, *volume_caption;
static lv_obj_t *brightness_sliders[2], *brightness_captions[2];
static unsigned brightness = 80, applied_brightness = 80;
static lv_obj_t *wifi_status, *scan_button, *scan_caption, *ap_list;
static lv_obj_t *selected_label, *password_status, *password_input;
static network_ap_t displayed_aps[NETWORK_MAX_APS], selected_ap;
static uint32_t displayed_generation;
static char language_options[TRANSLATION_LANGUAGE_COUNT * 32];

static void refresh(lv_timer_t *timer);
static void open_wifi(lv_event_t *event);
static void go_back(lv_event_t *event);
static void begin_translation(lv_event_t *event);

static void set_text(lv_obj_t *object, const char *value)
{
    if (strcmp(lv_label_get_text(object), value) != 0) lv_label_set_text(object, value);
}

static lv_obj_t *shape(lv_obj_t *parent, int x, int y, int w, int h, unsigned color, int radius)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return object;
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y, int width,
                      const lv_font_t *font, unsigned color)
{
    lv_obj_t *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_text(object, value);
    return object;
}

static lv_obj_t *button(lv_obj_t *parent, const char *value, int x, int y,
                        int width, int height, unsigned color, unsigned foreground)
{
    lv_obj_t *object = lv_button_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, 20, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(object, lv_color_darken(lv_color_hex(color), 20), LV_STATE_PRESSED);
    lv_obj_set_style_opa(object, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(object, lv_color_hex(foreground), 0);
    lv_obj_set_style_text_font(object, &lv_font_montserrat_20, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    if (height < 44) lv_obj_set_ext_click_area(object, (44 - height) / 2);
    lv_obj_t *caption = lv_label_create(object);
    lv_label_set_text(caption, value);
    lv_obj_center(caption);
    return object;
}

static void bunny(lv_obj_t *parent, int x, int y)
{
    shape(parent, x + 14, y, 19, 57, WHITE, 10);
    shape(parent, x + 48, y - 5, 19, 60, WHITE, 10);
    shape(parent, x + 20, y + 7, 7, 32, PINK, 4);
    shape(parent, x + 54, y + 2, 7, 34, PINK, 4);
    shape(parent, x, y + 31, 82, 68, WHITE, 32);
    shape(parent, x + 22, y + 57, 6, 8, INK, 3);
    shape(parent, x + 54, y + 57, 6, 8, INK, 3);
    shape(parent, x + 10, y + 71, 14, 7, PINK, 4);
    shape(parent, x + 58, y + 71, 14, 7, PINK, 4);
    text(parent, "w", x + 35, y + 65, 20, LV_FONT_DEFAULT, INK);
}

static void sparkle(lv_obj_t *parent, int x, int y, unsigned color)
{
    shape(parent, x + 5, y, 4, 14, color, 2);
    shape(parent, x, y + 5, 14, 4, color, 2);
}

static lv_obj_t *create_page(page_id_t id)
{
    lv_obj_t *page = shape(lv_screen_active(), 0, 0, 480, 480, CREAM, 0);
    lv_obj_set_style_bg_grad_color(page, lv_color_hex(0xF8EFE6), 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, 0);
    shape(page, 360, 55, 175, 156, PEACH, LV_RADIUS_CIRCLE);
    shape(page, -75, 330, 160, 170, MINT, LV_RADIUS_CIRCLE);
    shape(page, 388, 398, 120, 110, PINK, LV_RADIUS_CIRCLE);
    sparkle(page, 326, 101, 0xDBB66D);
    sparkle(page, 15, 260, 0xD8B2AB);
    shape(page, 439, 314, 6, 6, 0xC8DCC4, LV_RADIUS_CIRCLE);
    pages[id] = page;
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

static void show_page(page_id_t id)
{
    /* Dropdown lists live on the screen, outside their page container. */
    for (unsigned i = 0; i < SESSION_MODE_COUNT; ++i) {
        if (id != current_page && current_page == setup_pages[i]) {
            lv_dropdown_close(setups[i].source);
            lv_dropdown_close(setups[i].target);
            lv_dropdown_close(setups[i].voice);
        }
    }
    if (current_page == PAGE_PASSWORD && id != PAGE_PASSWORD) lv_textarea_set_text(password_input, "");
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        if (i == (unsigned)id) lv_obj_remove_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    current_page = id;
}

static void header(page_id_t id)
{
    lv_obj_t *wifi = button(pages[id], LV_SYMBOL_WIFI "  Wi-Fi off", 34, 24, 142, 36, MINT, GREEN);
    lv_obj_set_style_text_font(wifi, LV_FONT_DEFAULT, 0);
    wifi_captions[id] = lv_obj_get_child(wifi, 0);
    lv_obj_add_event_cb(wifi, open_wifi, LV_EVENT_CLICKED, NULL);
    lv_obj_t *brand = text(pages[id], "mosaico", 188, 32, 108, LV_FONT_DEFAULT, MUTED);
    lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_CENTER, 0);
    if (id == PAGE_HOME) {
        lv_obj_t *version = text(pages[id], "v6", 360, 33, 80, LV_FONT_DEFAULT, GREEN);
        lv_obj_set_style_text_align(version, LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        return_buttons[id] = button(pages[id], LV_SYMBOL_LEFT " Return", 346, 24, 100, 36, WHITE, INK);
        lv_obj_set_style_text_font(return_buttons[id], LV_FONT_DEFAULT, 0);
        lv_obj_add_event_cb(return_buttons[id], go_back, LV_EVENT_CLICKED, NULL);
    }
}

static bool session_busy(void)
{
    app_snapshot_t state;
    app_get_snapshot(&state);
    return session_requested() || (state.phase != APP_IDLE && state.phase != APP_ERROR);
}

static void set_disabled(lv_obj_t *object, bool disabled)
{
    if (disabled) lv_obj_add_state(object, LV_STATE_DISABLED);
    else lv_obj_remove_state(object, LV_STATE_DISABLED);
}

static void go_back(lv_event_t *event)
{
    (void)event;
    switch (current_page) {
    case PAGE_MODES:
        show_page(PAGE_HOME);
        break;
    case PAGE_MEETING_SETUP:
    case PAGE_TRAVEL_SETUP:
    case PAGE_MEDIA_SETUP:
        show_page(PAGE_MODES);
        break;
    case PAGE_TRANSLATE:
        /* Gate audio immediately; the session task retains ownership of all
         * asynchronous WebRTC/Agent cleanup before another start is allowed. */
        session_request(false);
        show_page(setup_pages[session_get_mode()]);
        break;
    case PAGE_PASSWORD:
        show_page(PAGE_WIFI);
        break;
    case PAGE_WIFI:
        show_page(wifi_origin);
        break;
    default:
        break;
    }
    refresh(NULL);
}

static void open_modes(lv_event_t *event)
{
    (void)event;
    show_page(PAGE_MODES);
    refresh(NULL);
}

static void open_wifi(lv_event_t *event)
{
    (void)event;
    if (current_page != PAGE_WIFI && current_page != PAGE_PASSWORD) wifi_origin = current_page;
    show_page(PAGE_WIFI);
    network_scan_start();
    refresh(NULL);
}

static void build_language_options(void)
{
    size_t used = 0;
    for (size_t i = 0; i < TRANSLATION_LANGUAGE_COUNT; ++i) {
        const translation_language_t *language = translation_language_at(i);
        int written = snprintf(language_options + used, sizeof(language_options) - used,
                               "%s%s", language->english_name,
                               i + 1 < TRANSLATION_LANGUAGE_COUNT ? "\n" : "");
        if (written < 0 || (size_t)written >= sizeof(language_options) - used) break;
        used += (size_t)written;
    }
}

static setup_t *active_setup(void)
{
    return &setups[session_get_mode()];
}

static void select_voice(setup_t *setup)
{
    const translation_language_t *target = translation_language_at(lv_dropdown_get_selected(setup->target));
    const translation_voice_t *voice = target ? translation_voice_for_language(
        target->code, lv_dropdown_get_selected(setup->voice)) : NULL;
    session_set_voice(voice ? voice->id : NULL);
}

static void build_voice_options(setup_t *setup)
{
    const translation_language_t *target = translation_language_at(lv_dropdown_get_selected(setup->target));
    if (!target) return;
    size_t used = 0, count = translation_voice_count_for_language(target->code);
    setup->voices[0] = '\0';
    for (size_t i = 0; i < count; ++i) {
        const translation_voice_t *voice = translation_voice_for_language(target->code, i);
        int written = snprintf(setup->voices + used, sizeof(setup->voices) - used,
                               "%s%s", voice->name, i + 1 < count ? "\n" : "");
        if (written < 0 || (size_t)written >= sizeof(setup->voices) - used) break;
        used += (size_t)written;
    }
    lv_dropdown_set_options(setup->voice, count ? setup->voices : "No voices available");
    lv_dropdown_set_selected(setup->voice, 0);
}

static void language_changed(lv_event_t *event)
{
    setup_t *setup = lv_event_get_user_data(event);
    if (lv_event_get_target(event) == setup->target) build_voice_options(setup);
    session_set_languages(lv_dropdown_get_selected(setup->source), lv_dropdown_get_selected(setup->target));
    select_voice(setup);
    subtitles_clear();
    refresh(NULL);
}

static void voice_changed(lv_event_t *event)
{
    select_voice(lv_event_get_user_data(event));
}

static void choose_mode(lv_event_t *event)
{
    if (session_busy()) return;
    session_mode_t mode = (session_mode_t)(uintptr_t)lv_event_get_user_data(event);
    setup_t *previous = active_setup();
    unsigned source = lv_dropdown_get_selected(previous->source);
    unsigned target = lv_dropdown_get_selected(previous->target);
    session_set_mode(mode);
    setup_t *setup = active_setup();
    if (previous != setup) {
        lv_dropdown_set_selected(setup->source, source);
        lv_dropdown_set_selected(setup->target, target);
        build_voice_options(setup);
    }
    session_set_languages(source, target);
    select_voice(setup);
    show_page(setup_pages[mode]);
    refresh(NULL);
}

static void layout_direction_languages(direction_t *direction, const translation_language_t *source,
                                      const translation_language_t *target, bool media, bool portable)
{
    lv_label_set_text_fmt(direction->flow, media ? "%s  " LV_SYMBOL_RIGHT "  %s" :
                          "%s\n" LV_SYMBOL_DOWN "\n%s",
                          source ? source->english_name : "Unknown language",
                          target ? target->english_name : "Unknown language");
    int width = media ? 392 : 172;
    int height = portable ? 104 : media ? 34 : 72;
    int top = portable ? 22 : 16;
    const lv_font_t *fonts[] = {&lv_font_montserrat_28, &lv_font_montserrat_20, LV_FONT_DEFAULT};
    const lv_font_t *font = LV_FONT_DEFAULT;
    lv_point_t size;
    for (unsigned i = !media && !portable ? 1 : 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        font = fonts[i];
        lv_text_get_size(&size, lv_label_get_text(direction->flow), font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x <= width && size.y <= height) break;
    }
    lv_obj_set_style_text_font(direction->flow, font, 0);
    lv_obj_set_style_text_letter_space(direction->flow, 0, 0);
    lv_obj_set_style_text_line_space(direction->flow, 0, 0);
    lv_obj_set_style_text_align(direction->flow, media ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(direction->flow, width, LV_SIZE_CONTENT);
    lv_obj_set_pos(direction->flow, 16, top + (size.y < height ? (height - size.y) / 2 : 0));
}

static void configure_directions(void)
{
    setup_t *setup = active_setup();
    const translation_language_t *source = translation_language_at(lv_dropdown_get_selected(setup->source));
    const translation_language_t *target = translation_language_at(lv_dropdown_get_selected(setup->target));
    session_mode_t mode = session_get_mode();
    bool portable = mode == SESSION_MODE_PORTABLE;
    bool media = mode == SESSION_MODE_MEDIA;
    set_text(translation_title, portable ? "Travel together" : media ? "Media translation" : "Live meeting");
    for (unsigned i = 0; i < 2; ++i) {
        direction_t *direction = &directions[i];
        if (media && i) lv_obj_add_flag(direction->root, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(direction->root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(direction->root, 28 + (int)i * 220, 153);
        lv_obj_set_width(direction->root, media && !i ? 424 : 204);
        layout_direction_languages(direction, i ? target : source, i ? source : target, media, portable);
        lv_obj_set_width(direction->hint, media ? 392 : 172);
        lv_obj_set_y(direction->hint, media ? 61 : 99);
        lv_obj_set_height(direction->hint, media ? 63 : 34);
        lv_obj_set_width(direction->state, media ? 392 : 172);
        lv_obj_set_width(direction->meter, media ? 392 : 172);
        if (portable) {
            set_text(direction->hint, "");
            lv_obj_add_flag(direction->hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            set_text(direction->hint, media ?
                     "Plug Mosaico into your Computer and select " MOSAICO_USB_DEVICE_NAME
                     " as the speaker, or connect it to your phone." :
                     i ? "Computer mic:\nChoose " MOSAICO_USB_DEVICE_NAME :
                         "Computer speaker:\nChoose " MOSAICO_USB_DEVICE_NAME);
            lv_obj_remove_flag(direction->hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void begin_translation(lv_event_t *event)
{
    (void)event;
    if (session_busy()) return;
    setup_t *setup = active_setup();
    unsigned source = lv_dropdown_get_selected(setup->source);
    unsigned target = lv_dropdown_get_selected(setup->target);
    if (source == target) {
        set_text(setup->hint, "Choose two different languages to translate.");
        return;
    }
    session_set_languages(source, target);
    select_voice(setup);
    configure_directions();
    subtitles_clear();
    session_set_travel_turn(SESSION_TRAVEL_NONE);
    session_request(true);
    show_page(PAGE_TRANSLATE);
    refresh(NULL);
}

static void start_stop(lv_event_t *event)
{
    (void)event;
    if (session_requested()) {
        session_request(false);
    } else if (!session_busy()) {
        subtitles_clear();
        session_request(true);
    }
    refresh(NULL);
}

static void direction_clicked(lv_event_t *event)
{
    if (session_get_mode() != SESSION_MODE_PORTABLE) return;
    if (!session_requested() && session_busy()) return;
    session_travel_turn_t turn = (session_travel_turn_t)(uintptr_t)lv_event_get_user_data(event);
    session_set_travel_turn(session_get_travel_turn() == turn ? SESSION_TRAVEL_NONE : turn);
    if (!session_requested()) {
        subtitles_clear();
        session_request(true);
    }
    refresh(NULL);
}

static lv_obj_t *slider(lv_obj_t *parent, int x, int y, int width, unsigned value, unsigned color)
{
    lv_obj_t *object = lv_slider_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, 8);
    lv_slider_set_range(object, 0, 100);
    lv_slider_set_value(object, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(object, lv_color_hex(LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), LV_PART_KNOB);
    lv_obj_set_style_pad_all(object, 5, LV_PART_KNOB);
    lv_obj_set_ext_click_area(object, 14);
    return object;
}

static void volume_changed(lv_event_t *event)
{
    unsigned value = lv_slider_get_value(lv_event_get_target(event));
    media_set_volume(value);
    lv_label_set_text_fmt(volume_caption, LV_SYMBOL_VOLUME_MAX "  %u%%", value);
}

static void sync_brightness(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        lv_slider_set_value(brightness_sliders[i], brightness, LV_ANIM_OFF);
        lv_label_set_text_fmt(brightness_captions[i], "Light  %u%%", brightness);
    }
}

static void brightness_changed(lv_event_t *event)
{
    brightness = lv_slider_get_value(lv_event_get_target(event));
    /* The minimum keeps the panel usable, including when on battery. */
    if (brightness < 10) brightness = 10;
    if (lv_event_get_code(event) == LV_EVENT_RELEASED && brightness != applied_brightness) {
        if (bsp_display_brightness_set((int)brightness) == ESP_OK) applied_brightness = brightness;
        else brightness = applied_brightness;
    }
    sync_brightness();
}

static void create_home(void)
{
    lv_obj_t *page = pages[PAGE_HOME];
    text(page, "Hello, Sunshine!", 36, 92, 360, &lv_font_montserrat_28, INK);
    home_date = text(page, "Your day, a little brighter.", 38, 135, 310, LV_FONT_DEFAULT, MUTED);
    home_clock = text(page, "--:--", 36, 169, 270, &lv_font_montserrat_40, GREEN);
    text(page, "Beijing Time / UTC+8", 40, 221, 300, LV_FONT_DEFAULT, MUTED);
    bunny(page, 334, 143);
    sparkle(page, 309, 191, 0xD6AF64);
    lv_obj_t *start = button(page, "Let's\ntranslate", 76, 246, 160, 160, GREEN, WHITE);
    lv_obj_set_style_radius(start, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(start, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_t *caption = lv_obj_get_child(start, 0);
    lv_obj_set_align(caption, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(caption, 12, 43);
    lv_obj_set_width(caption, 136);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *arrow = text(start, LV_SYMBOL_RIGHT, 60, 108, 40, &lv_font_montserrat_20, WHITE);
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(start, open_modes, LV_EVENT_CLICKED, NULL);

    lv_obj_t *battery = shape(page, 288, 266, 128, 128, BUTTER, LV_RADIUS_CIRCLE);
    home_battery = text(battery, LV_SYMBOL_BATTERY_FULL "  --%", 8, 32, 112, &lv_font_montserrat_20, INK);
    lv_obj_set_style_text_align(home_battery, LV_TEXT_ALIGN_CENTER, 0);
    home_battery_note = text(battery, "Checking battery", 12, 65, 104, LV_FONT_DEFAULT, INK);
    lv_obj_set_style_text_align(home_battery_note, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *light = shape(page, 64, 420, 352, 50, CREAM_PINK, LV_RADIUS_CIRCLE);
    brightness_captions[0] = text(light, "", 16, 18, 100, LV_FONT_DEFAULT, INK);
    brightness_sliders[0] = slider(light, 136, 26, 184, brightness, ROSE);
    lv_obj_set_style_bg_color(brightness_sliders[0], lv_color_hex(WHITE), LV_PART_MAIN);
    lv_slider_set_range(brightness_sliders[0], 10, 100);
    lv_obj_add_event_cb(brightness_sliders[0], brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brightness_sliders[0], brightness_changed, LV_EVENT_RELEASED, NULL);
}

static void create_modes(void)
{
    lv_obj_t *page = pages[PAGE_MODES];
    text(page, "Choose your adventure", 32, 91, 424, &lv_font_montserrat_28, INK);
    text(page, "A small device. A world of hello.", 34, 132, 410, LV_FONT_DEFAULT, MUTED);
    const unsigned colors[] = {MINT, PEACH, PINK};
    const char *titles[] = {"Live meeting mode", "Travel mode", "Media mode"};
    const char *notes[] = {"Two-way translation for live meetings", "Two languages, one little companion",
                           "Videos & clips from Computer or phone"};
    const char *icons[] = {LV_SYMBOL_CALL, LV_SYMBOL_AUDIO, LV_SYMBOL_PLAY};
    const session_mode_t order[] = {SESSION_MODE_MEDIA, SESSION_MODE_LIVE_MEETING, SESSION_MODE_PORTABLE};
    for (unsigned position = 0; position < SESSION_MODE_COUNT; ++position) {
        session_mode_t mode = order[position];
        int y = 170 + (int)position * 86;
        mode_buttons[mode] = button(page, "", 32, y, 416, 76, colors[mode], INK);
        shape(mode_buttons[mode], 16, 13, 50, 50, WHITE, 18);
        text(mode_buttons[mode], icons[mode], 28, 25, 40, &lv_font_montserrat_28, GREEN);
        text(mode_buttons[mode], titles[mode], 82, 13, 316, &lv_font_montserrat_20, INK);
        text(mode_buttons[mode], notes[mode], 82, 45, 316, LV_FONT_DEFAULT, INK);
        lv_obj_add_event_cb(mode_buttons[mode], choose_mode, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)mode);
    }
    mode_status = text(page, "Pick a mode to get started.", 40, 447, 400, LV_FONT_DEFAULT, MUTED);
    lv_obj_set_style_text_align(mode_status, LV_TEXT_ALIGN_CENTER, 0);
}

static lv_obj_t *dropdown(lv_obj_t *parent, int x, int y, int width, const char *options)
{
    lv_obj_t *object = lv_dropdown_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, 48);
    lv_obj_set_style_radius(object, 14, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(LINE), 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(CREAM), 0);
    lv_obj_set_style_text_color(object, lv_color_hex(INK), 0);
    lv_obj_set_style_text_font(object, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_pad_ver(object, 13, 0);
    lv_dropdown_set_options(object, options);
    lv_obj_t *list = lv_dropdown_get_list(object);
    lv_obj_set_style_text_font(list, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(CREAM), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(MINT), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(INK), LV_PART_SELECTED);
    lv_obj_set_style_max_height(list, 210, 0);
    return object;
}

static void create_setup(unsigned index)
{
    setup_t *setup = &setups[index];
    const char *titles[] = {"Meeting, made simple.", "A world between us", "Press play. We translate."};
    const char *notes[] = {"Two-way translation for your live meeting.", "Pick your languages and a voice.",
                           "Translate videos from your computer or phone."};
    const char *from[] = {"MEETING LANGUAGE", "FROM", "MEDIA LANGUAGE"};
    const char *to[] = {"YOUR LANGUAGE", "TO", "TRANSLATE TO"};
    const char *begin[] = {"Start live meeting", "Start travel mode", "Start media translation"};
    lv_obj_t *page = pages[setup_pages[index]];
    text(page, titles[index], 32, 90, 424, &lv_font_montserrat_28, INK);
    text(page, notes[index], 34, 130, 410, LV_FONT_DEFAULT, MUTED);
    lv_obj_t *card = shape(page, 28, 166, 424, 192, WHITE, 24);
    text(card, from[index], 16, 14, 180, LV_FONT_DEFAULT, MUTED);
    text(card, to[index], 222, 14, 180, LV_FONT_DEFAULT, MUTED);
    setup->source = dropdown(card, 16, 39, 186, language_options);
    setup->target = dropdown(card, 222, 39, 186, language_options);
    text(card, "VOICE", 16, 103, 392, LV_FONT_DEFAULT, MUTED);
    setup->voice = dropdown(card, 16, 128, 392, "Qingse (male)");
    lv_dropdown_set_selected(setup->source, translation_language_index("zh-CN"));
    lv_dropdown_set_selected(setup->target, translation_language_index("en-US"));
    build_voice_options(setup);
    lv_obj_add_event_cb(setup->source, language_changed, LV_EVENT_VALUE_CHANGED, setup);
    lv_obj_add_event_cb(setup->target, language_changed, LV_EVENT_VALUE_CHANGED, setup);
    lv_obj_add_event_cb(setup->voice, voice_changed, LV_EVENT_VALUE_CHANGED, setup);
    setup->hint = text(page, "", 42, 374, 396, LV_FONT_DEFAULT, MUTED);
    lv_obj_set_height(setup->hint, 36);
    lv_obj_set_style_text_align(setup->hint, LV_TEXT_ALIGN_CENTER, 0);
    char caption[64];
    snprintf(caption, sizeof(caption), "%s  " LV_SYMBOL_RIGHT, begin[index]);
    setup->begin = button(page, caption, 52, 421, 376, 44, GREEN, WHITE);
    lv_obj_add_event_cb(setup->begin, begin_translation, LV_EVENT_CLICKED, NULL);
}

static void create_translation(void)
{
    lv_obj_t *page = pages[PAGE_TRANSLATE];
    translation_title = text(page, "Live meeting", 28, 85, 280, &lv_font_montserrat_20, INK);
    translation_action = button(page, "Pause", 334, 77, 118, 36, WHITE, GREEN);
    lv_obj_set_style_text_font(translation_action, LV_FONT_DEFAULT, 0);
    translation_action_text = lv_obj_get_child(translation_action, 0);
    lv_obj_add_event_cb(translation_action, start_stop, LV_EVENT_CLICKED, NULL);
    translation_status = text(page, "Getting ready...", 30, 119, 420, LV_FONT_DEFAULT, MUTED);
    lv_label_set_long_mode(translation_status, LV_LABEL_LONG_MODE_DOTS);
    for (unsigned i = 0; i < 2; ++i) {
        direction_t *direction = &directions[i];
        direction->background = i ? PINK : MINT;
        direction->root = button(page, "", 28 + (int)i * 220, 153, 204, 179, direction->background, INK);
        lv_obj_set_style_bg_color(direction->root, lv_color_hex(i ? PINK : MINT), LV_STATE_PRESSED);
        direction->flow = text(direction->root, "", 16, 16, 172, &lv_font_montserrat_20, INK);
        direction->hint = text(direction->root, "", 16, 61, 172, LV_FONT_DEFAULT, INK);
        lv_obj_set_height(direction->hint, 63);
        lv_label_set_long_mode(direction->hint, LV_LABEL_LONG_MODE_WRAP);
        direction->state = text(direction->root, "", 16, 139, 172, LV_FONT_DEFAULT, GREEN);
        direction->meter = lv_bar_create(direction->root);
        lv_obj_set_pos(direction->meter, 16, 165);
        lv_obj_set_size(direction->meter, 172, 5);
        lv_obj_set_style_bg_color(direction->meter, lv_color_hex(WHITE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(direction->meter, lv_color_hex(i ? 0xBC7D88 : GREEN), LV_PART_INDICATOR);
        lv_obj_remove_flag(direction->meter, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(direction->root, direction_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)(i ? SESSION_TRAVEL_TARGET_LANGUAGE : SESSION_TRAVEL_SOURCE_LANGUAGE));
    }
    lv_obj_t *controls = shape(page, 28, 348, 424, 110, PALE_YELLOW, 24);
    volume_caption = text(controls, "", 18, 20, 110, LV_FONT_DEFAULT, INK);
    volume_slider = slider(controls, 166, 28, 226, media_get_volume(), GREEN);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(CREAM), LV_PART_MAIN);
    lv_obj_add_event_cb(volume_slider, volume_changed, LV_EVENT_VALUE_CHANGED, NULL);
    shape(controls, 18, 54, 388, 1, CREAM, 0);
    brightness_captions[1] = text(controls, "", 18, 75, 110, LV_FONT_DEFAULT, INK);
    brightness_sliders[1] = slider(controls, 174, 83, 210, brightness, ROSE);
    lv_obj_set_style_bg_color(brightness_sliders[1], lv_color_hex(CREAM), LV_PART_MAIN);
    lv_slider_set_range(brightness_sliders[1], 10, 100);
    lv_obj_add_event_cb(brightness_sliders[1], brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brightness_sliders[1], brightness_changed, LV_EVENT_RELEASED, NULL);
    lv_label_set_text_fmt(volume_caption, LV_SYMBOL_VOLUME_MAX "  %u%%", media_get_volume());
}

static void connect_selected(void)
{
    char password[65] = {0};
    if (selected_ap.secure) {
        const char *entered = lv_textarea_get_text(password_input);
        if (strlen(entered) < 8 || strlen(entered) > 63) {
            set_text(password_status, "Use 8-63 characters for the password.");
            return;
        }
        strcpy(password, entered);
    }
    esp_err_t result = network_connect(selected_ap.ssid, password);
    memset(password, 0, sizeof(password));
    lv_textarea_set_text(password_input, "");
    if (result == ESP_OK) show_page(PAGE_WIFI);
    else lv_label_set_text_fmt(password_status, "Could not connect (%s)", esp_err_to_name(result));
}

static void connect_pressed(lv_event_t *event)
{
    (void)event;
    connect_selected();
}

static void select_ap(lv_event_t *event)
{
    network_status_t network;
    network_get_status(&network);
    if (network.scanning || network.connection == NETWORK_CONNECTING) return;
    selected_ap = *(network_ap_t *)lv_event_get_user_data(event);
    if (!selected_ap.secure) {
        connect_selected();
    } else {
        lv_label_set_text(selected_label, selected_ap.ssid);
        set_text(password_status, "A little password, and we're in.");
        lv_textarea_set_text(password_input, "");
        show_page(PAGE_PASSWORD);
    }
}

static void password_ready(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) connect_selected();
}

static void scan_again(lv_event_t *event)
{
    (void)event;
    network_scan_start();
}

static void show_scan_results(void)
{
    size_t count = network_get_scan_results(displayed_aps, NETWORK_MAX_APS);
    lv_obj_clean(ap_list);
    if (!count) text(ap_list, "No networks yet. Try another scan.", 16, 16, 370, LV_FONT_DEFAULT, MUTED);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t *row = button(ap_list, "", 8, 8 + (int)i * 64, 392, 56, CREAM, INK);
        lv_obj_t *name = text(row, displayed_aps[i].ssid, 12, 7, 290, &lv_font_source_han_sans_sc_16_cjk, INK);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        text(row, displayed_aps[i].secure ? "Password protected" : "Open network", 12, 31, 248,
             LV_FONT_DEFAULT, MUTED);
        lv_obj_t *signal = text(row, "", 302, 20, 78, LV_FONT_DEFAULT, GREEN);
        lv_label_set_text_fmt(signal, "%d dBm", displayed_aps[i].rssi);
        lv_obj_add_event_cb(row, select_ap, LV_EVENT_CLICKED, &displayed_aps[i]);
    }
}

static void create_wifi(void)
{
    lv_obj_t *page = pages[PAGE_WIFI];
    text(page, "A little connection", 32, 91, 300, &lv_font_montserrat_28, INK);
    scan_button = button(page, "Scan", 346, 85, 100, 42, GREEN, WHITE);
    scan_caption = lv_obj_get_child(scan_button, 0);
    lv_obj_add_event_cb(scan_button, scan_again, LV_EVENT_CLICKED, NULL);
    wifi_status = text(page, "Choose your Wi-Fi network.", 34, 135, 410, &lv_font_source_han_sans_sc_16_cjk, MUTED);
    lv_obj_set_height(wifi_status, 38);
    ap_list = shape(page, 28, 183, 424, 272, WHITE, 24);
    lv_obj_add_flag(ap_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(ap_list, LV_DIR_VER);
    text(ap_list, "Tap Scan to find nearby networks.", 16, 16, 380, LV_FONT_DEFAULT, MUTED);

    page = pages[PAGE_PASSWORD];
    text(page, "Come on in", 32, 87, 400, &lv_font_montserrat_28, INK);
    selected_label = text(page, "Wi-Fi password", 34, 125, 410, &lv_font_source_han_sans_sc_16_cjk, MUTED);
    lv_label_set_long_mode(selected_label, LV_LABEL_LONG_MODE_DOTS);
    password_input = lv_textarea_create(page);
    lv_obj_set_pos(password_input, 28, 155);
    lv_obj_set_size(password_input, 286, 48);
    lv_obj_set_style_radius(password_input, 16, 0);
    lv_obj_set_style_border_color(password_input, lv_color_hex(LINE), 0);
    lv_obj_set_style_bg_color(password_input, lv_color_hex(WHITE), 0);
    lv_textarea_set_one_line(password_input, true);
    lv_textarea_set_password_mode(password_input, true);
    lv_textarea_set_max_length(password_input, 63);
    lv_textarea_set_placeholder_text(password_input, "Wi-Fi password");
    lv_obj_add_event_cb(password_input, password_ready, LV_EVENT_READY, NULL);
    lv_obj_t *connect = button(page, "Connect", 326, 155, 126, 48, GREEN, WHITE);
    lv_obj_add_event_cb(connect, connect_pressed, LV_EVENT_CLICKED, NULL);
    password_status = text(page, "", 34, 212, 410, LV_FONT_DEFAULT, MUTED);
    lv_obj_t *keyboard = lv_keyboard_create(page);
    lv_obj_set_align(keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(keyboard, 20, 246);
    lv_obj_set_size(keyboard, 440, 210);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(CREAM), 0);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(WHITE), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(MINT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keyboard, lv_color_hex(INK), LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, 10, LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard, password_input);
}

static void refresh_network(void)
{
    network_status_t network;
    network_get_status(&network);
    const char *caption = network.connection == NETWORK_CONNECTED ? LV_SYMBOL_WIFI "  Wi-Fi on" :
                          network.connection == NETWORK_CONNECTING ? LV_SYMBOL_WIFI "  Joining" :
                          LV_SYMBOL_WIFI "  Wi-Fi off";
    for (unsigned i = 0; i < PAGE_COUNT; ++i) set_text(wifi_captions[i], caption);
    if (current_page != PAGE_WIFI && current_page != PAGE_PASSWORD) return;
    char message[144];
    if (network.connection == NETWORK_CONNECTED) {
        snprintf(message, sizeof(message), "Connected to %s", network.ssid);
    } else if (network.connection == NETWORK_CONNECTING) {
        snprintf(message, sizeof(message), "Joining %s...", network.ssid);
    } else if (network.connection == NETWORK_FAILED) {
        if (network.disconnect_reason == WIFI_REASON_AUTH_FAIL ||
            network.disconnect_reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
            network.disconnect_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
            snprintf(message, sizeof(message), "Check the password and try again.");
        } else if (network.disconnect_reason == WIFI_REASON_NO_AP_FOUND) {
            snprintf(message, sizeof(message), "Network not found. Try another scan.");
        } else {
            snprintf(message, sizeof(message), "Connection failed (%d). Pick a network to retry.", network.disconnect_reason);
        }
    } else if (!network.scanning && network.scan_error != ESP_OK) {
        snprintf(message, sizeof(message), "Scan failed (%s). Please retry.", esp_err_to_name(network.scan_error));
    } else {
        snprintf(message, sizeof(message), "Choose your Wi-Fi network.");
    }
    set_text(wifi_status, message);
    set_text(scan_caption, network.scanning ? "Scanning" : "Scan");
    set_disabled(scan_button, network.scanning || network.connection == NETWORK_CONNECTING);
    if (network.scan_generation != displayed_generation) {
        displayed_generation = network.scan_generation;
        show_scan_results();
    }
}

static void refresh_home(void)
{
    if (current_page != PAGE_HOME) return;
    if (network_time_ready()) {
        time_t local = time(NULL) + 8 * 3600;
        struct tm calendar;
        gmtime_r(&local, &calendar);
        char value[64];
        strftime(value, sizeof(value), "%H:%M", &calendar);
        set_text(home_clock, value);
        strftime(value, sizeof(value), "%Y.%m.%d  %a", &calendar);
        set_text(home_date, value);
    } else {
        set_text(home_clock, "--:--");
        set_text(home_date, "Connect Wi-Fi to set the clock.");
    }
    ui_battery_t battery;
    ui_device_get_battery(&battery);
    char value[48];
    if (battery.available) {
        const char *icon = battery.charging ? LV_SYMBOL_CHARGE :
                           battery.percent > 75 ? LV_SYMBOL_BATTERY_FULL :
                           battery.percent > 40 ? LV_SYMBOL_BATTERY_2 : LV_SYMBOL_BATTERY_1;
        snprintf(value, sizeof(value), "%s  %u%%", icon, battery.percent);
        set_text(home_battery_note, battery.charging ? "Charging" : "Battery level");
    } else {
        snprintf(value, sizeof(value), LV_SYMBOL_BATTERY_EMPTY "  --%%");
        set_text(home_battery_note, "Battery unavailable");
    }
    set_text(home_battery, value);
}

static void refresh_translation(const app_snapshot_t *state)
{
    if (current_page != PAGE_TRANSLATE) return;
    bool portable = session_get_mode() == SESSION_MODE_PORTABLE;
    bool media = session_get_mode() == SESSION_MODE_MEDIA;
    bool running = state->phase == APP_RUNNING;
    set_text(translation_status, state->phase == APP_IDLE ? "Ready for another little conversation." : state->message);
    set_text(translation_action_text, session_requested() ? "Pause" : session_busy() ? "Stopping" : "Resume");
    set_disabled(translation_action, !session_requested() && session_busy());
    usb_audio_stats_t usb;
    usb_audio_get_stats(&usb);
    session_travel_turn_t turn = session_get_travel_turn();
    for (unsigned i = 0; i < 2; ++i) {
        bool selected = portable && (unsigned)turn == i + 1;
        unsigned color = i ? (selected ? 0xE8B9C2 : PINK) : (selected ? 0xBBD9B4 : MINT);
        if (directions[i].background != color) {
            directions[i].background = color;
            lv_obj_set_style_bg_color(directions[i].root, lv_color_hex(color), 0);
        }
        const char *caption = portable ? (selected ? "LISTENING" : "TAP TO SPEAK") :
                              !running ? "CONNECTING" : !usb.mounted ? "CONNECT DEVICE" :
                              !i && usb.host_muted ? (media ? "MEDIA MUTED" : "MEETING MUTED") :
                              media ? "MEDIA TO YOU" : i ? "YOU TO MEETING" : "MEETING TO YOU";
        if (!session_requested()) caption = "PAUSED";
        set_text(directions[i].state, caption);
        unsigned level = !running ? 0 : portable ? (selected ? media_get_portable_input_level() : 0) :
                         i ? media_get_portable_input_level() : usb.level;
        lv_bar_set_value(directions[i].meter, level, LV_ANIM_OFF);
    }
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    refresh_network();
    refresh_home();
    app_snapshot_t state;
    app_get_snapshot(&state);
    bool busy = session_busy();
    const char *hints[] = {"Choose Mosaico as your meeting's audio device.",
                           "Tap a language card to speak. Tap it again to pause.",
                           "Connect a computer or phone, then play your media."};
    for (unsigned i = 0; i < SESSION_MODE_COUNT; ++i) {
        set_disabled(mode_buttons[i], busy);
        setup_t *setup = &setups[i];
        bool same = lv_dropdown_get_selected(setup->source) == lv_dropdown_get_selected(setup->target);
        set_disabled(setup->source, busy);
        set_disabled(setup->target, busy);
        set_disabled(setup->voice, busy);
        set_disabled(setup->begin, busy || same);
        set_text(setup->hint, busy ? "Wrapping up translation. One little moment..." :
                 same ? "Choose two different languages to translate." : hints[i]);
    }
    set_text(mode_status, busy ? "Wrapping up translation. One little moment..." : "Pick a mode to get started.");
    refresh_translation(&state);
}

esp_err_t ui_init(void)
{
    if (!bsp_display_start() || !bsp_display_lock(-1)) return ESP_FAIL;
    /* TLSF limits each pool to the configured 64 KiB. Add three PSRAM pools
     * without enlarging the internal heap reserved for LVGL/Wi-Fi/LCD. */
    void *page_memory = heap_caps_malloc(192 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_mem_pool_t first = page_memory ? lv_mem_add_pool(page_memory, 64 * 1024) : NULL;
    lv_mem_pool_t second = first ? lv_mem_add_pool((uint8_t *)page_memory + 64 * 1024, 64 * 1024) : NULL;
    lv_mem_pool_t third = second ? lv_mem_add_pool((uint8_t *)page_memory + 128 * 1024, 64 * 1024) : NULL;
    if (!third) {
        if (second) lv_mem_remove_pool(second);
        if (first) lv_mem_remove_pool(first);
        heap_caps_free(page_memory);
        bsp_display_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        create_page((page_id_t)i);
        header((page_id_t)i);
    }
    build_language_options();
    create_home();
    create_modes();
    for (unsigned i = 0; i < SESSION_MODE_COUNT; ++i) create_setup(i);
    create_translation();
    create_wifi();
    sync_brightness();
    if (bsp_display_brightness_set((int)brightness) != ESP_OK) {
        ESP_LOGW("UI", "Initial brightness could not be applied");
    }
    session_set_languages(translation_language_index("zh-CN"), translation_language_index("en-US"));
    select_voice(&setups[0]);
    show_page(PAGE_HOME);
    refresh(NULL);
    lv_timer_create(refresh, 150, NULL);
    bsp_display_unlock();
    ui_device_init();
    return ESP_OK;
}

void ui_show_wifi_settings(void)
{
    if (bsp_display_lock(-1)) {
        open_wifi(NULL);
        bsp_display_unlock();
    }
}
