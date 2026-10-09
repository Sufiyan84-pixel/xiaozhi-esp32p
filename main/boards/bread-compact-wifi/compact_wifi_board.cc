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
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// 1 = SH1106 driver (1.3" OLED), 0 = SSD1306 driver (0.96" OLED)
// Agar display par text 2 pixel khisak ke dikhe ya garbled ho, ye value badal do.
#ifndef OLED_USE_SH1106
#define OLED_USE_SH1106 1
#endif

#if OLED_USE_SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"
#define OLED_I2C_SPEED_HZ   (100 * 1000)

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
        static LampController lamp(LAMP_GPIO);
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
            I2S_STD_SLOT_LEFT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN,
            I2S_STD_SLOT_LEFT);
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
