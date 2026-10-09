#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

// ============================================================================
//  Compact WiFi Board  -  Premium Edition
//
//   * Boot splash   : OLED par dithered fade-in / fade-out animation
//   * Smooth lamp   : PWM + gamma + ease-in/out (soft on/off) + brightness control
//   * Robust OLED   : 7-bit address detect + retry + soft-fail (crash nahi hoga)
// ============================================================================

// ------------------------------ Tuning macros -------------------------------

// 1 = SH1106 driver (1.3" OLED), 0 = SSD1306 driver (0.96" OLED)
// Display par text 2 pixel khisak ke dikhe ya garbled ho to ye value badlo.
#ifndef OLED_USE_SH1106
#define OLED_USE_SH1106 1
#endif

// 1 = boot par OLED animation chalegi (~2 sec). Garbled dikhe to 0 kar do.
#ifndef BOOT_SPLASH
#define BOOT_SPLASH 1
#endif
#define SPLASH_NAME     "ALIYA"             // sirf A-Z aur space
#define SPLASH_TAGLINE  "VOICE ASSISTANT"   // sirf A-Z aur space

// 1 = lamp smooth fade (LED / MOSFET ke liye). 0 = purana seedha on/off.
// DHYAN: agar lamp RELAY se chal rahi hai to ise 0 rakho (relay PWM se kharab hota hai).
#ifndef LAMP_SMOOTH_FADE
#define LAMP_SMOOTH_FADE 1
#endif
// 1 = lamp active-low hai (GPIO LOW par jalti hai)
#ifndef LAMP_ACTIVE_LOW
#define LAMP_ACTIVE_LOW 0
#endif

#if OLED_USE_SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"
#define OLED_I2C_SPEED_HZ   (100 * 1000)

// ============================================================================
//  SMOOTH LAMP  (PWM + gamma 2.2 + exponential ease)
// ============================================================================
#if LAMP_SMOOTH_FADE
class SmoothLamp {
public:
    explicit SmoothLamp(gpio_num_t gpio) : gpio_(gpio) {
        ledc_timer_config_t timer = {};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.timer_num = LEDC_TIMER_2;
        timer.freq_hz = 5000;
        timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer));

        ledc_channel_config_t ch = {};
        ch.gpio_num = gpio_;
        ch.speed_mode = LEDC_LOW_SPEED_MODE;
        ch.channel = LEDC_CHANNEL_5;
        ch.intr_type = LEDC_INTR_DISABLE;
        ch.timer_sel = LEDC_TIMER_2;
        ch.duty = DutyFor(0.0f);
        ch.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&ch));

        xTaskCreate(&SmoothLamp::TaskEntry, "lamp_fade", 3072, this, 3, &task_);
        RegisterTools();
        ESP_LOGI(TAG, "💡 Smooth lamp ready (GPIO %d)", (int)gpio_);
    }

private:
    static constexpr float kFadeInTauMs = 130.0f;    // on: ~0.7 sec
    static constexpr float kFadeOutTauMs = 180.0f;   // off: ~1.0 sec
    static constexpr int kStepMs = 10;

    gpio_num_t gpio_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> on_{false};
    std::atomic<int> brightness_{100};
    float level_ = 0.0f;                              // abhi ki (perceptual) roshni 0..1

    static uint32_t DutyFor(float level) {
        constexpr uint32_t kMax = (1u << 10) - 1;
        float g = std::pow(level, 2.2f);              // gamma: aankh ko smooth lage
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
#if LAMP_ACTIVE_LOW
        g = 1.0f - g;
#endif
        return (uint32_t)(g * kMax + 0.5f);
    }

    void Apply() {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5, DutyFor(level_));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_5);
    }

    void Kick() { xTaskNotifyGive(task_); }

    static void TaskEntry(void* arg) { static_cast<SmoothLamp*>(arg)->Run(); }

    void Run() {
        for (;;) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            for (;;) {
                float target = on_.load() ? brightness_.load() / 100.0f : 0.0f;
                float diff = target - level_;
                if (std::fabs(diff) < 0.004f) {       // pahunch gaye
                    level_ = target;
                    Apply();
                    break;
                }
                float tau = diff > 0.0f ? kFadeInTauMs : kFadeOutTauMs;
                level_ += diff * (1.0f - std::exp(-(float)kStepMs / tau));
                Apply();
                vTaskDelay(pdMS_TO_TICKS(kStepMs));
            }
        }
    }

    void RegisterTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.lamp.get_state",
            "Get the power state and brightness (0-100) of the lamp",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                std::string s = std::string("{\"power\": ") + (on_.load() ? "true" : "false") +
                                ", \"brightness\": " + std::to_string(brightness_.load()) + "}";
                return s;
            });

        mcp_server.AddTool("self.lamp.turn_on",
            "Turn on the lamp with a smooth fade-in",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (brightness_.load() <= 0) brightness_.store(100);
                on_.store(true);
                Kick();
                return true;
            });

        mcp_server.AddTool("self.lamp.turn_off",
            "Turn off the lamp with a smooth fade-out",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                on_.store(false);
                Kick();
                return true;
            });

        mcp_server.AddTool("self.lamp.set_brightness",
            "Set the lamp brightness from 0 to 100 percent (smooth transition)",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int b = properties["brightness"].value<int>();
                if (b < 0) b = 0;
                if (b > 100) b = 100;
                brightness_.store(b);
                on_.store(b > 0);
                Kick();
                return true;
            });
    }
};
#endif  // LAMP_SMOOTH_FADE

// ============================================================================
//  BOOT SPLASH  (1-bit dithered fade, OLED page format mein seedha draw)
// ============================================================================
#if BOOT_SPLASH
namespace {

// 5x7 font, A-Z (har glyph ki 7 rows, 5 bit)
const uint8_t kFont5x7[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},  // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},  // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},  // C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},  // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},  // E
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},  // F
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},  // G
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},  // H
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},  // I
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},  // J
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},  // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},  // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},  // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},  // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},  // O
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},  // P
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},  // Q
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},  // R
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},  // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},  // T
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},  // U
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},  // V
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},  // W
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},  // X
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},  // Y
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},  // Z
};

// 4x4 Bayer matrix (0..15): dithered fade ke liye
const uint8_t kBayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};

class BootSplash {
public:
    static constexpr int W = 128;
    static constexpr int H = 64;
    static constexpr int kPages = H / 8;

    explicit BootSplash(esp_lcd_panel_handle_t panel) : panel_(panel) {
        std::memset(name_layer_, 0, sizeof(name_layer_));
        std::memset(tag_layer_, 0, sizeof(tag_layer_));
        Layout();
    }

    void Run() {
        // 1) naam dithered fade-in
        for (int lvl = 2; lvl <= 16; lvl += 2) {
            Compose(lvl, 0, 0, 0);
            Present(name_p0_, name_p1_);
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        // 2) neeche ki line beech se dono taraf phailti hai (ease-out)
        constexpr int kSweepSteps = 10;
        for (int s = 1; s <= kSweepSteps; s++) {
            float t = (float)s / kSweepSteps;
            float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            int half = (int)(e * 52.0f);
            Compose(16, 0, half, 0);
            Present(line_p0_, line_p1_);
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        // 3) tagline fade-in
        for (int lvl = 4; lvl <= 16; lvl += 4) {
            Compose(16, lvl, 52, 0);
            Present(tag_p0_, tag_p1_);
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        // 4) thoda ruko
        vTaskDelay(pdMS_TO_TICKS(450));
        // 5) sab kuch fade-out
        for (int lvl = 14; lvl >= 0; lvl -= 2) {
            Compose(lvl, lvl, lvl > 0 ? 52 : 0, lvl);
            Present(name_p0_, tag_p1_);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        // 6) screen saaf
        std::memset(fb_, 0, sizeof(fb_));
        Present(0, kPages - 1);
    }

private:
    esp_lcd_panel_handle_t panel_;
    uint8_t fb_[W * kPages] = {};
    uint8_t name_layer_[W * kPages];
    uint8_t tag_layer_[W * kPages];
    int line_y_ = 40;
    int name_p0_ = 0, name_p1_ = 0, line_p0_ = 0, line_p1_ = 0, tag_p0_ = 0, tag_p1_ = 0;

    static void SetPx(uint8_t* layer, int x, int y) {
        if (x < 0 || x >= W || y < 0 || y >= H) return;
        layer[(y >> 3) * W + x] |= (uint8_t)(1u << (y & 7));
    }
    static bool GetPx(const uint8_t* layer, int x, int y) {
        return (layer[(y >> 3) * W + x] >> (y & 7)) & 1;
    }

    // text ko layer par scale s ke saath, x ke beech mein draw karo
    static void DrawText(uint8_t* layer, const char* text, int y0, int s) {
        int n = (int)std::strlen(text);
        if (n <= 0) return;
        int gap = s;
        int total_w = n * 5 * s + (n - 1) * gap;
        int x = (W - total_w) / 2;
        for (int c = 0; c < n; c++) {
            char ch = text[c];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            if (ch >= 'A' && ch <= 'Z') {
                const uint8_t* g = kFont5x7[ch - 'A'];
                for (int row = 0; row < 7; row++) {
                    for (int col = 0; col < 5; col++) {
                        if (g[row] & (1u << (4 - col))) {
                            for (int dy = 0; dy < s; dy++)
                                for (int dx = 0; dx < s; dx++)
                                    SetPx(layer, x + col * s + dx, y0 + row * s + dy);
                        }
                    }
                }
            }
            x += 5 * s + gap;
        }
    }

    void Layout() {
        // naam ka scale: jitna bada ho sake (max 4) par screen mein aaye
        int n = (int)std::strlen(SPLASH_NAME);
        int s = 4;
        while (s > 1 && n * 5 * s + (n - 1) * s > W - 4) s--;
        int block_h = 7 * s + 5 + 1 + 8 + 7;       // naam + gap + line + gap + tagline
        int y0 = (H - block_h) / 2;
        if (y0 < 0) y0 = 0;
        int name_y = y0;
        line_y_ = name_y + 7 * s + 6;
        int tag_y = line_y_ + 8;

        DrawText(name_layer_, SPLASH_NAME, name_y, s);
        DrawText(tag_layer_, SPLASH_TAGLINE, tag_y, 1);

        name_p0_ = name_y >> 3;
        name_p1_ = (name_y + 7 * s - 1) >> 3;
        line_p0_ = line_y_ >> 3;
        line_p1_ = (line_y_ + 1) >> 3;
        tag_p0_ = tag_y >> 3;
        tag_p1_ = (tag_y + 6) >> 3;
        if (tag_p1_ >= kPages) tag_p1_ = kPages - 1;
    }

    // frame banao: har layer ka alag dither level (0..16), line ki aadhi lambai (px)
    void Compose(int name_lvl, int tag_lvl, int line_half, int line_lvl) {
        std::memset(fb_, 0, sizeof(fb_));
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                bool on = false;
                if (name_lvl > 0 && GetPx(name_layer_, x, y) && kBayer4[y & 3][x & 3] < name_lvl) on = true;
                if (tag_lvl > 0 && GetPx(tag_layer_, x, y) && kBayer4[y & 3][x & 3] < tag_lvl) on = true;
                if (on) SetPx(fb_, x, y);
            }
        }
        if (line_half > 0) {
            int lvl = line_lvl > 0 ? line_lvl : 16;
            int cx = W / 2;
            for (int x = cx - line_half; x <= cx + line_half; x++) {
                for (int dy = 0; dy < 2; dy++) {
                    if (kBayer4[(line_y_ + dy) & 3][x & 3] < lvl) SetPx(fb_, x, line_y_ + dy);
                }
            }
        }
    }

    // page p0..p1 OLED par bhejo
    void Present(int p0, int p1) {
        if (p0 < 0) p0 = 0;
        if (p1 >= kPages) p1 = kPages - 1;
        if (p1 < p0) return;
        esp_lcd_panel_draw_bitmap(panel_, 0, p0 * 8, W, (p1 + 1) * 8, fb_ + p0 * W);
    }
};

}  // namespace
#endif  // BOOT_SPLASH

// ============================================================================
//  BOARD
// ============================================================================
class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
        ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d, speed=%d Hz)",
                 DISPLAY_SDA_PIN, DISPLAY_SCL_PIN, OLED_I2C_SPEED_HZ);
    }

    // ESP-IDF i2c_master_probe() aur dev_addr dono 7-bit address lete hain.
    uint8_t DetectDisplayAddress() {
        const uint8_t candidates[] = { 0x3C, 0x3D };

        vTaskDelay(pdMS_TO_TICKS(100));
        for (int attempt = 1; attempt <= 5; attempt++) {
            for (uint8_t addr : candidates) {
                if (i2c_master_probe(display_i2c_bus_, addr, 100) == ESP_OK) {
                    ESP_LOGI(TAG, "Display detected at 7-bit address 0x%02X (attempt %d)",
                             addr, attempt);
                    return addr;
                }
            }
            ESP_LOGW(TAG, "No display found (attempt %d/5)", attempt);
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        ESP_LOGE(TAG, "No OLED found at 0x3C / 0x3D!");
        ESP_LOGI(TAG, "Performing full I2C bus scan for debugging...");
        bool found_any = false;
        for (uint8_t a = 1; a < 127; a++) {
            if (i2c_master_probe(display_i2c_bus_, a, 50) == ESP_OK) {
                ESP_LOGI(TAG, "  I2C device found -> 7-bit: 0x%02X", a);
                found_any = true;
            }
        }
        if (!found_any) {
            ESP_LOGE(TAG, "I2C bus is empty. Check SDA(GPIO%d)/SCL(GPIO%d) wiring and 3V3 power.",
                     DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);
        }
        return 0;
    }

    void InitializeSsd1306Display() {
        uint8_t dev_addr = DetectDisplayAddress();
        if (dev_addr == 0) {
            ESP_LOGE(TAG, "Display not available, using NoDisplay fallback.");
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = dev_addr,
            .scl_speed_hz = OLED_I2C_SPEED_HZ,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        esp_err_t io_ret = esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_);
        if (io_ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c failed: 0x%x", io_ret);
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#if OLED_USE_SH1106
        const char* drv_name = "SH1106";
        ESP_LOGI(TAG, "Install OLED driver (%s)", drv_name);
        esp_err_t pnl_ret = esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_);
#else
        const char* drv_name = "SSD1306";
        ESP_LOGI(TAG, "Install OLED driver (%s)", drv_name);
        esp_err_t pnl_ret = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
#endif
        if (pnl_ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_new_panel_%s failed: 0x%x", drv_name, pnl_ret);
            display_ = new NoDisplay();
            return;
        }
        ESP_LOGI(TAG, "%s driver installed", drv_name);

        // ESP_ERROR_CHECK ki jagah soft-fail: OLED mein problem ho to device crash na kare
        if (esp_lcd_panel_reset(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Display reset failed");
            display_ = new NoDisplay();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));

        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        esp_lcd_panel_invert_color(panel_, false);

        ESP_LOGI(TAG, "Turning display on");
        if (esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to turn display on");
            display_ = new NoDisplay();
            return;
        }

#if BOOT_SPLASH
        // Splash sirf 128x64 par. Mirror pehle hi laga do taaki animation seedhi dikhe.
        if (DISPLAY_WIDTH == 128 && DISPLAY_HEIGHT == 64) {
            esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
            ESP_LOGI(TAG, "✨ Boot splash");
            BootSplash splash(panel_);
            splash.Run();
        }
#endif

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_LOGI(TAG, "OLED initialized successfully!");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
            if (display_) {
                display_->ShowNotification("(^_^) Sun raha hoon...");
            }
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
            if (display_) {
                display_->ShowNotification("(o_o) Ruk gaya");
            }
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) volume = 100;
            codec->SetOutputVolume(volume);
            if (display_) {
                display_->ShowNotification("(^_^) Volume: " + std::to_string(volume));
            }
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            if (display_) {
                display_->ShowNotification("(^o^) Max Volume!");
            }
        });
        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            if (display_) {
                display_->ShowNotification("(-_-) Volume: " + std::to_string(volume));
            }
        });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            if (display_) {
                display_->ShowNotification("(x_x) Muted");
            }
        });
    }

    void InitializeTools() {
#if LAMP_SMOOTH_FADE
        static SmoothLamp lamp(LAMP_GPIO);
#else
        static LampController lamp(LAMP_GPIO);
#endif
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
