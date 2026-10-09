#include "no_audio_codec.h"

#include <esp_log.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#define TAG "NoAudioCodec"

// 1 = boot par ek baar speaker mein 1 kHz ki beep bajegi (speaker/amp ka hardware test).
// Beep sunai de -> speaker + amp + wiring sahi hain. Na aaye -> hardware problem.
// Test ho jaye to 0 kar do.
#define SPEAKER_SELFTEST_BEEP 1

NoAudioCodec::~NoAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}

NoAudioCodecDuplex::NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}


NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    ESP_LOGI(TAG, "=== Simplex Audio Init ===");
    ESP_LOGI(TAG, "Input rate: %d Hz, Output rate: %d Hz", input_sample_rate_, output_sample_rate_);
    ESP_LOGI(TAG, "SPK: BCLK=%d, WS=%d, DOUT=%d", spk_bclk, spk_ws, spk_dout);
    ESP_LOGI(TAG, "MIC: SCK=%d, WS=%d, DIN=%d", mic_sck, mic_ws, mic_din);

    // Speaker channel
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Speaker channel initialized");

    // Mic channel
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Mic channel initialized");
    ESP_LOGI(TAG, "=== Simplex channels created ===");
}

int NoAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);

    // ---- DEBUG: Speaker ko asli audio mil raha hai ya nahi ----
    // peak_since = pichle log ke baad ka sabse bada sample (sirf ek block ka nahi)
    static int write_count = 0;
    static int32_t peak_since = 0;
    for (int i = 0; i < samples; i++) {
        int32_t abs_val = abs(data[i]);
        if (abs_val > peak_since) peak_since = abs_val;
    }
    if (write_count++ % 100 == 0) {
        ESP_LOGI(TAG, "SPK write: samples=%d, peak=%ld, vol=%d", samples, (long)peak_since, output_volume_);
        peak_since = 0;
    }

    std::vector<int32_t> buffer(samples);
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    for (int i = 0; i < samples; i++) {
        int64_t temp = int64_t(data[i]) * volume_factor;
        if (temp > INT32_MAX) {
            buffer[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
            buffer[i] = INT32_MIN;
        } else {
            buffer[i] = static_cast<int32_t>(temp);
        }
    }

    size_t bytes_written;
    esp_err_t ret = i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPK write failed: %d", ret);
        return 0;
    }
    return bytes_written / sizeof(int32_t);
}

int NoAudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    constexpr uint32_t kReadTimeoutMs = 200;

    std::vector<int32_t> bit32_buffer(samples);
    esp_err_t ret = i2s_channel_read(rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t), &bytes_read, kReadTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MIC read failed: %d", ret);
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);

    // ---- Mic processing: glitch filter + DC remove + auto gain (AGC) ----
    // (Read sirf ek hi audio task se call hota hai, isliye static state safe hai)
    static int32_t dc_est = 0;       // DC offset estimate (24-bit units)
    static int32_t prev_v = 0;       // pichla sahi sample
    static int glitch_run = 0;       // lagatar kitne sample glitch mane gaye
    static float gain = 8.0f;        // AGC gain (1.0 = no gain)
    static int read_count = 0;
    static int glitch_total = 0;

    constexpr int32_t kGlitchJump = 3000000;  // ek sample mein itna bada jump = noise/bit-error
    constexpr float kTargetPeak = 9000.0f;    // output ka target peak (16-bit)
    constexpr float kMinGain = 0.5f;
    constexpr float kMaxGain = 40.0f;         // zyada awaaz chahiye to ye badhao
    constexpr float kNoiseFloor = 40.0f;      // isse dheemi awaaz = chup, gain mat badhao

    int32_t raw_max = 0;
    int32_t block_peak = 0;                   // cleaned peak (24-bit units)

    for (int i = 0; i < samples; i++) {
        int32_t r = bit32_buffer[i];
        int32_t ar = (r == INT32_MIN) ? INT32_MAX : (r < 0 ? -r : r);
        if (ar > raw_max) raw_max = ar;

        int32_t v = r >> 8;                   // 32-bit slot -> 24-bit signed sample

        int32_t jump = v - prev_v;
        if (jump < 0) jump = -jump;
        if (jump > kGlitchJump && glitch_run < 8) {
            v = prev_v;                       // glitch: pichla sample hold karo
            glitch_run++;
            glitch_total++;
        } else {
            glitch_run = 0;
        }
        prev_v = v;

        dc_est += (v - dc_est) / 256;         // slow DC tracker (~10 Hz high-pass)
        int32_t y = v - dc_est;
        bit32_buffer[i] = y;

        int32_t ay = y < 0 ? -y : y;
        if (ay > block_peak) block_peak = ay;
    }

    float peak16 = block_peak / 256.0f;       // gain ke bina peak (16-bit units)
    if (samples > 0) {
        if (peak16 * gain > kTargetPeak) {
            gain = kTargetPeak / (peak16 > 1.0f ? peak16 : 1.0f);   // tez attack
        } else if (peak16 > kNoiseFloor && peak16 * gain < kTargetPeak * 0.8f) {
            gain *= 1.03f;                                          // dheema release
        }
        if (gain < kMinGain) gain = kMinGain;
        if (gain > kMaxGain) gain = kMaxGain;
    }

    const float scale = gain / 256.0f;
    for (int i = 0; i < samples; i++) {
        float f = (float)bit32_buffer[i] * scale;
        if (f > 32767.0f) f = 32767.0f;
        if (f < -32767.0f) f = -32767.0f;
        dest[i] = (int16_t)f;
    }

    // ---- DEBUG: Mic status (har ~1 second) ----
    if (read_count++ % 100 == 0) {
        ESP_LOGI(TAG, "MIC raw max=%ld, clean peak=%d, gain x10=%d, glitches=%d",
                 (long)raw_max, (int)peak16, (int)(gain * 10.0f), glitch_total);
        glitch_total = 0;
    }
    return samples;
}

void NoAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) return;
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
        ESP_LOGI(TAG, "MIC enabled");
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
        ESP_LOGI(TAG, "MIC disabled");
    }
    AudioCodec::EnableInput(enable);
}

#if SPEAKER_SELFTEST_BEEP
// Speaker hardware test: seedha I2S par 1 kHz ki ~0.3 sec beep (server/volume se independent)
static void PlaySelfTestBeep(i2s_chan_handle_t tx, int sample_rate) {
    constexpr int kChunk = 480;
    const int total = sample_rate / 3;
    const float amp = 0.25f * 2147483647.0f;
    std::vector<int32_t> buf(kChunk);
    ESP_LOGI(TAG, "Speaker self-test beep (1 kHz)...");
    int n = 0;
    while (n < total) {
        int len = std::min(kChunk, total - n);
        for (int i = 0; i < len; i++) {
            buf[i] = (int32_t)(amp * std::sin(2.0f * 3.14159265f * 1000.0f * (float)(n + i) / (float)sample_rate));
        }
        size_t written = 0;
        i2s_channel_write(tx, buf.data(), len * sizeof(int32_t), &written, 1000);
        n += len;
    }
    ESP_LOGI(TAG, "Self-test beep done");
}
#endif

void NoAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) return;
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
        ESP_LOGI(TAG, "SPK enabled");
#if SPEAKER_SELFTEST_BEEP
        static bool beep_done = false;
        if (!beep_done) {
            beep_done = true;
            PlaySelfTestBeep(tx_handle_, output_sample_rate_);
        }
#endif
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
        ESP_LOGI(TAG, "SPK disabled");
    }
    AudioCodec::EnableOutput(enable);
}

// Delegating constructor: calls the main constructor with default slot mask
NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_din) 
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout, I2S_STD_SLOT_LEFT, mic_sck, mic_din) {
    // All initialization is handled by the delegated constructor
}

NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(1), I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    tx_chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    tx_chan_cfg.auto_clear_after_cb = true;
    tx_chan_cfg.auto_clear_before_cb = false;
    tx_chan_cfg.intr_priority = 0;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, NULL));

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = spk_slot_mask,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));
#if SOC_I2S_SUPPORTS_PDM_RX
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(0), I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle_));
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = mic_sck,
            .din = mic_din,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_rx_cfg));
#else
    ESP_LOGE(TAG, "PDM is not supported");
#endif
    ESP_LOGI(TAG, "Simplex channels created");
}

int NoAudioCodecSimplexPdm::Read(int16_t* dest, int samples) {
    size_t bytes_read;

    if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }

    samples = bytes_read / sizeof(int16_t);
    if (input_gain_ > 0) {
        int gain_factor = (int)input_gain_;
        for (int i = 0; i < samples; i++) {
            int32_t amplified = dest[i] * gain_factor;
            dest[i] = (amplified > INT16_MAX) ? INT16_MAX : (amplified < -INT16_MAX) ? -INT16_MAX : (int16_t)amplified;
        }
    }
    return samples;
}
