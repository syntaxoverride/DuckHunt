#pragma once
/*
 * ESP32-2432S028 "Cheap Yellow Display" (CYD)
 * Panel marking: TPM408-2.8 (ILI9341 240x320) + XPT2046 resistive touch
 * Display init: LovyanGFX LGFX_SUNTON_ESP32_2432S028
 */

#define SCR_W 240
#define SCR_H 320

/* TFT pins (LovyanGFX Sunton profile owns these) */
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   21

/* XPT2046 touch (separate VSPI bus) */
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

#define BOOT_PIN 0
