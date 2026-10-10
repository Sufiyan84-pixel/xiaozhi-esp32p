#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <lvgl.h>

// ============================================================================
//  Robot face states (8 moods)
// ============================================================================
enum class RobotState {
    Idle,        // Normal eyes, smile
    Listening,   // Wide eyes, small mouth
    Speaking,    // Mouth open/close
    Thinking,    // Eyes up
    Happy,       // Squinted eyes, big smile
    Sad,         // Droopy eyes, frown
    Sleeping,    // Closed eyes
    Surprised    // Big eyes, open mouth
};

class OledDisplay : public LvglDisplay {
private:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    // Layout objects
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* content_left_ = nullptr;
    lv_obj_t* content_right_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* emotion_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* notification_label_ = nullptr;
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* mute_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;

    // Robot face canvas
    lv_obj_t* face_canvas_ = nullptr;
    lv_color_t* canvas_buffer_ = nullptr;
    RobotState current_robot_state_ = RobotState::Idle;

    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    void SetupUI_128x64();
    void SetupUI_128x32();

    // Robot drawing functions
    void DrawRobotFace(RobotState state, int anim_frame);
    void DrawEye(int x, int y, int w, int h, bool blink);
    void DrawMouth(int x, int y, int w, int h, int open);

public:
    OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, bool mirror_x, bool mirror_y);
    ~OledDisplay();

    virtual void SetupUI() override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetTheme(Theme* theme) override;
    virtual bool IsMonochrome() const override { return true; }
    void SetPowerSaveMode(bool on) override;

    // === Premium Idle Screen ===
    void ShowPremiumIdleScreen();

    // === Robot Animations ===
    void SetRobotState(RobotState state);
    void StartBlinkAnimation();
    void StartSpeakingAnimation();
    void StartListeningAnimation();
    void StartThinkingAnimation();
    void StopAllAnimations();
};

#endif  // OLED_DISPLAY_H
