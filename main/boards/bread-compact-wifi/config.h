#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================================
//  BREAD COMPACT WIFI - ADVANCED CONFIG
//  Board: ESP32-S3 + INMP441 Mic + MAX98357 Speaker + SH1106 OLED
// ============================================================

// ---------- Audio Sample Rates ----------
// Server 24000 bhejta hai, isliye output bhi 24000 (resampling nahi hoga)
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// ---------- I2S Mode ----------
// Simplex = Mic aur Speaker ke alag I2S channels (INMP441 + MAX98357 ke liye)
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

// INMP441 Mic Pins
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

// MAX98357 Speaker Pins
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

#else

// Duplex I2S (agar dono ek hi channel par ho)
#define AUDIO_I2S_GPIO_WS GPIO_NUM_4
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7

#endif

// ---------- LED & Buttons ----------
#define BUILTIN_LED_GPIO        GPIO_NUM_48
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_47
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

// ---------- Display (SH1106 128x64) ----------
#define DISPLAY_SDA_PIN GPIO_NUM_41
#define DISPLAY_SCL_PIN GPIO_NUM_42
#define DISPLAY_WIDTH   128
#define DISPLAY_HEIGHT  64
#define SH1106

// Display orientation
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

// ---------- MCP Lamp Test ----------
#define LAMP_GPIO GPIO_NUM_18

#endif // _BOARD_CONFIG_H_
