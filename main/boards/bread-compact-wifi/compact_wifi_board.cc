#include "no_audio_codec.h"

#include <esp_log.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#define TAG "NoAudioCodec"

// ============================================================================
//  NoAudioCodec  -  ASR-friendly Audio Engine
//
//  MIC     : warm-up skip -> glitch filter -> DC remove -> 80 Hz high-pass
//            -> RMS-based slow AGC (smooth, no pumping, instant clip-protection)
//            -> soft limiter            (noise gate optional, default OFF)
//  SPEAKER : 120 Hz high-pass -> smooth volume -> soft limiter -> fade-in
//
//  Design: speech-recognition ko "natural" awaaz pasand hai. Isliye mic par
//  halka aur smooth processing hai (gain dheere badalta hai), gate band hai
//  (gate shabdon ka shuru kaat deta tha), aur clipping se turant bachav hai.
// ============================================================================

#define SPEAKER_BOOT_CHIME 1
#define SPEAKER_CHIME_LEVEL 0.14f
#define SPEAKER_OUTPUT_SCALE 0.4f    // awaaz phate to kam karo, dheemi lage to badhao (max 0.6)

// 1 = halka noise gate. Default 0: gate shabd ka pehla hissa dabata hai, ASR ko nuksaan.
#define MIC_NOISE_GATE 0

namespace {

constexpr int kMicWarmupMs = 120;            // INMP441 wake-up ~85 ms
std::atomic<int> g_mic_warmup_samples{0};
std::atomic<bool> g_mic_reset{false};
std::atomic<int> g_spk_fade_pos{1 << 30};    // bada number = fade khatam

constexpr float kPi = 3.14159265f;

// Soft limiter: knee tak seedha, uske upar tanh se naram compression (output +-1 ke andar)
inline float SoftLimit(float x, float knee) {
    float a = x < 0.0f ? -x : x;
    if (a <= knee) return x;
    float over = (a - knee) / (1.0f - knee);
    float y = knee + (1.0f - knee) * std::tanh(over);
    return x < 0.0f ? -y : y;
}

// 1-pole high-pass ka coefficient
inline float HighPassCoef(float fc, float fs) {
    float rc = 1.0f / (2.0f * kPi * fc);
    return rc / (rc + 1.0f / fs);
}

}  // namespace

NoAudioCodec::~NoAudioCodec() {
    if (rx_handle_ != nullptr) ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    if (tx_handle_ != nullptr) ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
}

// ============================================================================
//  Duplex Constructor
// ============================================================================
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
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

// ============================================================================
//  Simplex Constructor
// ============================================================================
NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    ESP_LOGI(TAG, "=== Simplex Audio Init ===");
    ESP_LOGI(TAG, "Input: %d Hz | Output: %d Hz", input_sample_rate_, output_sample_rate_);
    ESP_LOGI(TAG, "SPK  -> BCLK=%d, WS=%d, DOUT=%d", spk_bclk, spk_ws, spk_dout);
    ESP_LOGI(TAG, "MIC  -> SCK=%d, WS=%d, DIN=%d", mic_sck, mic_ws, mic_din);

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
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
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

// ============================================================================
//  SPEAKER WRITE
// ============================================================================
int NoAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);

    // ---- State (Write sirf ek audio task se call hota hai) ----
    static float hp_x1 = 0.0f, hp_y1 = 0.0f;   // 120 Hz high-pass memory
    static float vol_gain = -1.0f;             // smooth volume (negative = abhi init nahi)
    static int write_count = 0;
    static int32_t peak_since = 0;

    const float fs = output_sample_rate_ > 0 ? (float)output_sample_rate_ : 24000.0f;
    const float hp_a = HighPassCoef(120.0f, fs);
    const float vol_step = 1.0f - std::exp(-1.0f / (fs * 0.02f));   // ~20 ms volume glide
    const int fade_total = (int)(fs * 0.010f);                      // 10 ms fade-in

    int vol = output_volume_;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    const float vol_target = (float)std::pow((double)vol / 100.0, 2.0);
    if (vol_gain < 0.0f) vol_gain = vol_target;

    int fade_pos = g_spk_fade_pos.load();

    std::vector<int32_t> buffer(samples);
    for (int i = 0; i < samples; i++) {
        int32_t abs_in = std::abs((int)data[i]);
        if (abs_in > peak_since) peak_since = abs_in;

        float x = (float)data[i] * (1.0f / 32768.0f);

        // 120 Hz high-pass: DC aur bahut neeche ka bass hatao (chhota speaker wahan phatta hai)
        float y = hp_a * (hp_y1 + x - hp_x1);
        hp_x1 = x;
        hp_y1 = y;

        // smooth volume (volume badalne par click nahi)
        vol_gain += (vol_target - vol_gain) * vol_step;
        float o = y * vol_gain;

        // naram limiter + max awaaz ka ceiling
        o = SoftLimit(o, 0.5f) * SPEAKER_OUTPUT_SCALE;

        // fade-in (speaker chalu hote hi pop na aaye)
        if (fade_pos < fade_total) {
            o *= (float)fade_pos / (float)fade_total;
            fade_pos++;
        }

        buffer[i] = (int32_t)(o * 2147483520.0f);
    }
    if (fade_pos > fade_total) fade_pos = fade_total;
    g_spk_fade_pos.store(fade_pos);

    if (write_count++ % 200 == 0) {
        ESP_LOGI(TAG, "🔊 SPK | samples=%d | in-peak=%ld | vol=%d", samples, (long)peak_since, output_volume_);
        peak_since = 0;
    }

    size_t bytes_written;
    esp_err_t ret = i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ SPK write failed: %d", ret);
        return 0;
    }
    return bytes_written / sizeof(int32_t);
}

// ============================================================================
//  MIC READ
// ============================================================================
int NoAudioCodec::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    constexpr uint32_t kReadTimeoutMs = 200;

    std::vector<int32_t> bit32_buffer(samples);
    esp_err_t ret = i2s_channel_read(rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t), &bytes_read, kReadTimeoutMs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠️ MIC read failed: %d", ret);
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);

    // ---- Warm-up: mic ke shuru ka kharab data phenk do ----
    {
        int warm = g_mic_warmup_samples.load();
        if (warm > 0) {
            g_mic_warmup_samples.store(warm > samples ? warm - samples : 0);
            for (int i = 0; i < samples; i++) dest[i] = 0;
            return samples;
        }
    }

    // ---- Tuning (16-bit units: 32768 = full scale) ----
    constexpr int32_t kGlitchJump = 3000000;    // ek sample mein itna bada jump = glitch (24-bit units)
    constexpr float kTargetRms = 2200.0f;       // output ka target RMS (~ -23.5 dBFS)
    constexpr float kMinGain = 0.3f;
    constexpr float kMaxGain = 80.0f;           // +38 dB
    constexpr float kIdleGain = 16.0f;          // chup rehne par gain yahan tak laut aata hai (+24 dB)
        constexpr float kGainDownPer10ms = 0.70f;   // gain ghatna: -3 dB / 10 ms (tez)
    constexpr float kIdleDriftPer10ms = 1.0116f;// chup mein idle gain ki taraf: 0.1 dB / 10 ms
    constexpr float kPeakCeiling = 26000.0f;    // output peak isse upar nahi jaana chahiye

    // ---- State (Read sirf ek audio task se call hota hai) ----
    static int32_t dc_est = 0;
    static int32_t prev_v = 0;
    static int glitch_run = 0;
    static float hp_x1 = 0.0f, hp_y1 = 0.0f;
    static float level = 0.0f;           // awaaz ka smooth RMS (gain se pehle)
    static float gain = kIdleGain;
    static float gate = 1.0f;
    static float noise_est = 25.0f;      // room noise RMS ka andaza (sessions ke beech yaad rehta hai)
    static float win_min = 1.0e9f;
    static float win_acc = 0.0f;
    static int read_count = 0;
    static int glitch_total = 0;

    if (g_mic_reset.exchange(false)) {   // naya listening session
        dc_est = 0;
        prev_v = 0;
        glitch_run = 0;
        hp_x1 = 0.0f;
        hp_y1 = 0.0f;
        level = 0.0f;
        gain = kIdleGain;
        gate = 1.0f;
    }

    const float fs = input_sample_rate_ > 0 ? (float)input_sample_rate_ : 16000.0f;
    const float hp_a = HighPassCoef(80.0f, fs);

    int32_t raw_max = 0;
    float block_peak = 0.0f;
    double sum_sq = 0.0;
    std::vector<float> clean(samples);

    for (int i = 0; i < samples; i++) {
        int32_t r = bit32_buffer[i];
        int32_t ar = (r == INT32_MIN) ? INT32_MAX : (r < 0 ? -r : r);
        if (ar > raw_max) raw_max = ar;

        int32_t v = r >> 8;                      // 32-bit slot -> 24-bit signed

        // glitch filter
        int32_t jump = v - prev_v;
        if (jump < 0) jump = -jump;
        if (jump > kGlitchJump && glitch_run < 8) {
            v = prev_v;
            glitch_run++;
            glitch_total++;
        } else {
            glitch_run = 0;
        }
        prev_v = v;

        // DC hatao + 80 Hz high-pass
        dc_est += (v - dc_est) / 256;
        float yf = (float)(v - dc_est);
        float yh = hp_a * (hp_y1 + yf - hp_x1);
        hp_x1 = yf;
        hp_y1 = yh;

        clean[i] = yh;
        float ay = yh < 0.0f ? -yh : yh;
        if (ay > block_peak) block_peak = ay;
        sum_sq += (double)yh * (double)yh;
    }

    const float blk = samples > 0 ? (float)samples / 160.0f : 1.0f;   // 160 samples = 10 ms
    auto coef = [blk](float c) { return 1.0f - std::pow(1.0f - c, blk); };

    const float gain_prev = gain;
    const float gate_prev = gate;
    const float peak16 = block_peak / 256.0f;                          // gain se pehle peak
    const float rms16 = samples > 0 ? (float)std::sqrt(sum_sq / samples) / 256.0f : 0.0f;

    if (samples > 0) {
        // ---- room noise ka andaza (har ~3 sec ka sabse dheema block) ----
        if (rms16 < win_min) win_min = rms16;
        if (rms16 < noise_est) noise_est += (rms16 - noise_est) * coef(0.2f);
        win_acc += blk;
        if (win_acc >= 300.0f) {
            if (win_min > noise_est) noise_est += (win_min - noise_est) * 0.5f;
            win_min = 1.0e9f;
            win_acc = 0.0f;
        }

        // ---- awaaz ka level: upar jaldi (~20 ms), neeche dheere (~300 ms) ----
        if (rms16 > level) level += (rms16 - level) * coef(0.5f);
        else level += (rms16 - level) * coef(0.03f);

        float speech_floor = noise_est * 2.5f;
        if (speech_floor < 6.0f) speech_floor = 6.0f;
        if (speech_floor > 100.0f) speech_floor = 100.0f;

        if (level > speech_floor) {
            // ---- AGC: gain ko target RMS ki taraf, rate-limited (smooth) ----
            float g_target = kTargetRms / level;
            if (g_target < kMinGain) g_target = kMinGain;
            if (g_target > kMaxGain) g_target = kMaxGain;
            if (g_target < gain) {
                float down = gain * std::pow(kGainDownPer10ms, blk);
                gain = down > g_target ? down : g_target;
            } else if (rms16 > speech_floor) {
                // gain badhna: jitna door utna tez (0.3 .. 3 dB / 10 ms), sirf asli awaaz wale block par.
                // Shabdon ke beech ke gap mein gain ruka rehta hai (noise pumping nahi hogi).
                float err_db = 20.0f * std::log10(g_target / gain);
                float step_db = 0.2f * err_db;
                if (step_db < 0.3f) step_db = 0.3f;
                if (step_db > 3.0f) step_db = 3.0f;
                float up = gain * std::pow(10.0f, step_db * blk / 20.0f);
                gain = up < g_target ? up : g_target;
            }
        } else {
            // ---- chup: gain dheere dheere idle value ki taraf ----
            if (gain > kIdleGain) {
                float d = gain / std::pow(kIdleDriftPer10ms, blk);
                gain = d > kIdleGain ? d : kIdleGain;
            } else if (gain < kIdleGain) {
                float u = gain * std::pow(kIdleDriftPer10ms, blk);
                gain = u < kIdleGain ? u : kIdleGain;
            }
        }

        // ---- clip se turant bachav: peak * gain ceiling ke andar ----
        if (peak16 * gain > kPeakCeiling) {
            gain = kPeakCeiling / (peak16 > 1.0f ? peak16 : 1.0f);
            if (gain < 0.05f) gain = 0.05f;
        }

#if MIC_NOISE_GATE
        float gate_target = (level < noise_est * 1.8f) ? 0.7f : 1.0f;
        gate += (gate_target - gate) * coef(gate_target < gate ? 0.05f : 0.6f);
#endif
    }

    // ---- Output: per-sample gain glide + soft limiter ----
    const float total_prev = gain_prev * gate_prev;
    const float total_new = gain * gate;
    const float norm = 1.0f / (256.0f * 32768.0f);   // 24-bit units -> +-1.0
    for (int i = 0; i < samples; i++) {
        float t = samples > 1 ? (float)i / (float)(samples - 1) : 1.0f;
        float g = total_prev + (total_new - total_prev) * t;
        float o = SoftLimit(clean[i] * g * norm, 0.85f) * 32767.0f;
        if (o > 32767.0f) o = 32767.0f;
        if (o < -32767.0f) o = -32767.0f;
        dest[i] = (int16_t)o;
    }

    // ---- Log (har ~1 second). %f nano-printf mein nahi chalta, isliye int ----
    if (read_count++ % 100 == 0) {
        ESP_LOGI(TAG, "🎤 MIC | raw=%ld | rms=%d | peak=%d | noise=%d | gain x10=%d | glitches=%d",
                 (long)raw_max, (int)rms16, (int)peak16, (int)noise_est, (int)(gain * 10.0f), glitch_total);
        glitch_total = 0;
    }

    return samples;
}

// ============================================================================
//  Enable / Disable
// ============================================================================
void NoAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) return;
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
        g_mic_warmup_samples.store(input_sample_rate_ * kMicWarmupMs / 1000);
        g_mic_reset.store(true);
        ESP_LOGI(TAG, "🎤 MIC enabled (warm-up %d ms)", kMicWarmupMs);
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
        ESP_LOGI(TAG, "🎤 MIC disabled");
    }
    AudioCodec::EnableInput(enable);
}

#if SPEAKER_BOOT_CHIME
// Cute boot chime: C5 - E5 - G5 - C6, bell jaisi naram awaaz (ye speaker test bhi hai)
static void PlayBootChime(i2s_chan_handle_t tx, int sample_rate) {
    static const float kNotes[] = {523.25f, 659.25f, 783.99f, 1046.50f};
    constexpr int kNoteCount = 4;
    constexpr float kNoteSec = 0.11f;
    constexpr float kTailSec = 0.20f;
    constexpr int kChunk = 480;

    const float fs = (float)sample_rate;
    const int total = (int)(fs * (kNoteCount * kNoteSec + kTailSec));
    std::vector<int32_t> buf(kChunk);

    ESP_LOGI(TAG, "🔔 Boot chime");
    int n = 0;
    while (n < total) {
        int len = std::min(kChunk, total - n);
        for (int i = 0; i < len; i++) {
            float t = (float)(n + i) / fs;
            float sum = 0.0f;
            for (int k = 0; k < kNoteCount; k++) {
                float age = t - k * kNoteSec;
                if (age < 0.0f) continue;
                float attack = age < 0.004f ? age / 0.004f : 1.0f;
                float env = attack * std::exp(-age * 14.0f);
                float w = 2.0f * kPi * kNotes[k] * age;
                sum += env * (std::sin(w) + 0.3f * std::sin(2.0f * w));
            }
            buf[i] = (int32_t)(sum * SPEAKER_CHIME_LEVEL * 2147483520.0f);
        }
        size_t written = 0;
        i2s_channel_write(tx, buf.data(), len * sizeof(int32_t), &written, 1000);
        n += len;
    }
}
#endif

void NoAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) return;
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
        g_spk_fade_pos.store(0);                // agla Write fade-in se shuru hoga
        ESP_LOGI(TAG, "🔊 SPK enabled");
#if SPEAKER_BOOT_CHIME
        static bool chime_done = false;
        if (!chime_done) {
            chime_done = true;
            PlayBootChime(tx_handle_, output_sample_rate_);
        }
#endif
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
        ESP_LOGI(TAG, "🔊 SPK disabled");
    }
    AudioCodec::EnableOutput(enable);
}

// ============================================================================
//  PDM Variant
// ============================================================================
NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_din) 
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout, I2S_STD_SLOT_LEFT, mic_sck, mic_din) {}

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
        .clk_cfg = { .sample_rate_hz = (uint32_t)output_sample_rate_, .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
        .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = I2S_SLOT_MODE_MONO, .slot_mask = spk_slot_mask, .ws_width = I2S_DATA_BIT_WIDTH_32BIT, .ws_pol = false, .bit_shift = true },
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = spk_bclk, .ws = spk_ws, .dout = spk_dout, .din = I2S_GPIO_UNUSED, .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false } },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));

#if SOC_I2S_SUPPORTS_PDM_RX
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(0), I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle_));
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .clk = mic_sck, .din = mic_din, .invert_flags = { .clk_inv = false } },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_rx_cfg));
#else
    ESP_LOGE(TAG, "PDM is not supported");
#endif
    ESP_LOGI(TAG, "PDM Simplex channels created");
}

int NoAudioCodecSimplexPdm::Read(int16_t* dest, int samples) {
    size_t bytes_read;
    if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "PDM Read Failed!");
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
