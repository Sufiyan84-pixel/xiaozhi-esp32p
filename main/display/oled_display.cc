#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <string>
#include <ctime>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <esp_heap_caps.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

// Robot face canvas
#define FACE_W 64
#define FACE_H 64

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }
}

void OledDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }
}

OledDisplay::~OledDisplay() {
    if (content_ != nullptr) lv_obj_del(content_);
    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) lv_obj_del(container_);

    if (canvas_buffer_ != nullptr) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
    }

    if (panel_ != nullptr) esp_lcd_panel_del(panel_);
    if (panel_io_ != nullptr) esp_lcd_panel_io_del(panel_io_);
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }
void OledDisplay::Unlock() { lvgl_port_unlock(); }

// ============================================================================
//  SetChatMessage - Premium Fade-In
// ============================================================================
void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) return;

    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    lv_anim_delete(chat_message_label_, nullptr);
    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, content_right_);
            lv_anim_set_values(&a, 0, 255);
            lv_anim_set_duration(&a, 250);
            lv_anim_set_exec_cb(&a, [](void* obj, int32_t value) {
                lv_obj_set_style_opa((lv_obj_t*)obj, value, 0);
            });
            lv_anim_start(&a);
        }
    }
}

// ============================================================================
//  SetupUI_128x64 - Premium Layout + Robot Face
// ============================================================================
void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    // Container
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    // Top bar
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    // Status bar
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    // Content with robot face
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_main_place(content_, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_cross_place(content_, LV_FLEX_ALIGN_CENTER, 0);

    // Robot face canvas
    face_canvas_ = lv_canvas_create(content_);
    canvas_buffer_ = (lv_color_t*)heap_caps_malloc(FACE_W * FACE_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (canvas_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Canvas buffer allocation failed!");
    } else {
        lv_canvas_set_buffer(face_canvas_, canvas_buffer_, FACE_W, FACE_H, LV_COLOR_FORMAT_A8);
        lv_obj_set_size(face_canvas_, FACE_W, FACE_H);
    }

    // Chat message (below face)
    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - 4);

    // Draw initial idle face
    SetRobotState(RobotState::Idle);

    // Low battery popup
    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void OledDisplay::SetupUI_128x32() {
    SetupUI_128x64();
}

// ============================================================================
//  ROBOT FACE DRAWING
// ============================================================================
void OledDisplay::DrawEye(int x, int y, int w, int h, bool blink) {
    if (face_canvas_ == nullptr || canvas_buffer_ == nullptr) return;

    lv_layer_t layer;
    lv_canvas_init_layer(face_canvas_, &layer);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = w / 2;

    lv_area_t area = {x, y, x + w, y + (blink ? h / 4 : h)};
    lv_draw_rect(&layer, &dsc, &area);

    lv_canvas_finish_layer(face_canvas_, &layer);
}

void OledDisplay::DrawMouth(int x, int y, int w, int h, int open) {
    if (face_canvas_ == nullptr || canvas_buffer_ == nullptr) return;

    lv_layer_t layer;
    lv_canvas_init_layer(face_canvas_, &layer);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 3;

    lv_area_t area = {x, y, x + w, y + h + open};
    lv_draw_rect(&layer, &dsc, &area);

    lv_canvas_finish_layer(face_canvas_, &layer);
}

void OledDisplay::DrawRobotFace(RobotState state, int anim_frame) {
    if (face_canvas_ == nullptr || canvas_buffer_ == nullptr) return;

    // Clear canvas
    lv_canvas_fill_bg(face_canvas_, lv_color_black(), LV_OPA_COVER);

    int eye_w = 16, eye_h = 16;
    int eye_y = 16;
    int left_eye_x = 8;
    int right_eye_x = 40;
    int mouth_x = 20, mouth_y = 44;
    int mouth_w = 24, mouth_h = 6;

    switch (state) {
        case RobotState::Idle: {
            int blink = (anim_frame % 60 < 4) ? 1 : 0;
            DrawEye(left_eye_x, eye_y, eye_w, eye_h, blink);
            DrawEye(right_eye_x, eye_y, eye_w, eye_h, blink);
            DrawMouth(mouth_x, mouth_y, mouth_w, mouth_h, 0);
            break;
        }
        case RobotState::Listening: {
            DrawEye(left_eye_x, eye_y - 2, eye_w + 2, eye_h + 2, false);
            DrawEye(right_eye_x, eye_y - 2, eye_w + 2, eye_h + 2, false);
            DrawMouth(mouth_x + 4, mouth_y, mouth_w - 8, mouth_h, 0);
            break;
        }
        case RobotState::Speaking: {
            int open = (anim_frame % 20 < 10) ? 8 : 0;
            DrawEye(left_eye_x, eye_y, eye_w, eye_h, false);
            DrawEye(right_eye_x, eye_y, eye_w, eye_h, false);
            DrawMouth(mouth_x, mouth_y, mouth_w, mouth_h, open);
            break;
        }
        case RobotState::Thinking: {
            DrawEye(left_eye_x, eye_y - 3, eye_w - 2, eye_h - 2, false);
            DrawEye(right_eye_x, eye_y - 3, eye_w - 2, eye_h - 2, false);
            DrawMouth(mouth_x + 8, mouth_y, mouth_w - 16, mouth_h, 0);
            break;
        }
        case RobotState::Happy: {
            DrawEye(left_eye_x, eye_y + 4, eye_w, 6, false);
            DrawEye(right_eye_x, eye_y + 4, eye_w, 6, false);
            DrawMouth(mouth_x - 4, mouth_y, mouth_w + 8, mouth_h + 2, 4);
            break;
        }
        case RobotState::Sad: {
            DrawEye(left_eye_x, eye_y + 2, eye_w, eye_h - 4, false);
            DrawEye(right_eye_x, eye_y + 2, eye_w, eye_h - 4, false);
            DrawMouth(mouth_x + 4, mouth_y + 4, mouth_w - 8, mouth_h - 2, 0);
            break;
        }
        case RobotState::Sleeping: {
            DrawEye(left_eye_x, eye_y + 6, eye_w, 2, false);
            DrawEye(right_eye_x, eye_y + 6, eye_w, 2, false);
            DrawMouth(mouth_x + 8, mouth_y, mouth_w - 16, mouth_h, 0);
            break;
        }
        case RobotState::Surprised: {
            DrawEye(left_eye_x - 2, eye_y - 2, eye_w + 4, eye_h + 4, false);
            DrawEye(right_eye_x - 2, eye_y - 2, eye_w + 4, eye_h + 4, false);
            DrawMouth(mouth_x + 4, mouth_y, mouth_w - 8, mouth_h + 6, 8);
            break;
        }
    }
}

// ============================================================================
//  ROBOT STATE + ANIMATIONS
// ============================================================================
void OledDisplay::SetRobotState(RobotState state) {
    DisplayLockGuard lock(this);
    current_robot_state_ = state;
    DrawRobotFace(state, 0);
}

void OledDisplay::StartBlinkAnimation() {
    if (face_canvas_ == nullptr) return;

    static lv_anim_t blink;
    lv_anim_init(&blink);
    lv_anim_set_var(&blink, face_canvas_);
    lv_anim_set_values(&blink, 0, 60);
    lv_anim_set_duration(&blink, 3000);
    lv_anim_set_repeat_count(&blink, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&blink, [](void* obj, int32_t value) {
        auto* self = static_cast<OledDisplay*>(lv_obj_get_user_data((lv_obj_t*)obj));
        if (self) self->DrawRobotFace(self->current_robot_state_, value);
    });
    lv_anim_start(&blink);
}

void OledDisplay::StartSpeakingAnimation() {
    if (face_canvas_ == nullptr) return;

    static lv_anim_t talk;
    lv_anim_init(&talk);
    lv_anim_set_var(&talk, face_canvas_);
    lv_anim_set_values(&talk, 0, 20);
    lv_anim_set_duration(&talk, 400);
    lv_anim_set_playback_duration(&talk, 400);
    lv_anim_set_repeat_count(&talk, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&talk, [](void* obj, int32_t value) {
        auto* self = static_cast<OledDisplay*>(lv_obj_get_user_data((lv_obj_t*)obj));
        if (self) self->DrawRobotFace(RobotState::Speaking, value);
    });
    lv_anim_start(&talk);
}

void OledDisplay::StartListeningAnimation() {
    SetRobotState(RobotState::Listening);
}

void OledDisplay::StartThinkingAnimation() {
    SetRobotState(RobotState::Thinking);
}

void OledDisplay::StopAllAnimations() {
    if (face_canvas_ != nullptr) {
        lv_anim_delete(face_canvas_, nullptr);
    }
}

// ============================================================================
//  SetEmotion - Robot State Mapping
// ============================================================================
void OledDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return;

    if (strcmp(emotion, "happy") == 0) SetRobotState(RobotState::Happy);
    else if (strcmp(emotion, "sad") == 0) SetRobotState(RobotState::Sad);
    else if (strcmp(emotion, "sleepy") == 0) SetRobotState(RobotState::Sleeping);
    else if (strcmp(emotion, "surprised") == 0) SetRobotState(RobotState::Surprised);
    else if (strcmp(emotion, "thinking") == 0) SetRobotState(RobotState::Thinking);
    else SetRobotState(RobotState::Idle);
}

// ============================================================================
//  PREMIUM IDLE SCREEN
// ============================================================================
void OledDisplay::ShowPremiumIdleScreen() {
    SetRobotState(RobotState::Idle);
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void OledDisplay::SetPowerSaveMode(bool on) {
    if (panel_) {
        Settings settings("wifi", false);
        if (settings.GetBool("power_save_display_off", false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}
