// The Orb OS — the Waveshare ESP32-S3-Touch-AMOLED-1.75, as this firmware wires it.
//
// ONE pin map. The values come from src/config.h, the file the Arduino firmware runs on
// real Orbs, so the two builds cannot disagree about a pin during the port. Every one was
// also checked against Waveshare's own board package (waveshare/esp32_s3_touch_amoled_1_75
// 3.0.1) and agrees with it. This header only gives them ESP-IDF's types.
#pragma once
#include "driver/gpio.h"
#include "config.h"

// --- Display: CO5300 AMOLED, 466 x 466, QSPI --------------------------------------
#define BOARD_LCD_H_RES        SCREEN_W
#define BOARD_LCD_V_RES        SCREEN_H
#define BOARD_LCD_CS           ((gpio_num_t)PIN_LCD_CS)
#define BOARD_LCD_PCLK         ((gpio_num_t)PIN_LCD_SCLK)
#define BOARD_LCD_D0           ((gpio_num_t)PIN_LCD_D0)
#define BOARD_LCD_D1           ((gpio_num_t)PIN_LCD_D1)
#define BOARD_LCD_D2           ((gpio_num_t)PIN_LCD_D2)
#define BOARD_LCD_D3           ((gpio_num_t)PIN_LCD_D3)
#define BOARD_LCD_RST          ((gpio_num_t)PIN_LCD_RST)
#define BOARD_LCD_TE           ((gpio_num_t)PIN_LCD_TE)   // tearing-effect output; not used yet
#define BOARD_LCD_COL_OFFSET   LCD_COL_OFFSET
#define BOARD_LCD_ROW_OFFSET   LCD_ROW_OFFSET

// --- Shared I2C: touch, IMU, RTC, PMIC, codec --------------------------------------
#define BOARD_I2C_SDA          ((gpio_num_t)PIN_I2C_SDA)
#define BOARD_I2C_SCL          ((gpio_num_t)PIN_I2C_SCL)

// --- Audio: ES8311 over I2S, speaker amp -------------------------------------------
#define BOARD_I2S_MCLK         ((gpio_num_t)PIN_I2S_MCLK)
#define BOARD_I2S_BCLK         ((gpio_num_t)PIN_I2S_BCLK)
#define BOARD_I2S_WS           ((gpio_num_t)PIN_I2S_LRCLK)
#define BOARD_I2S_DOUT         ((gpio_num_t)PIN_I2S_DOUT)
#define BOARD_I2S_DIN          ((gpio_num_t)PIN_I2S_DIN)
#define BOARD_AUDIO_PA         ((gpio_num_t)PIN_AUDIO_PA)

// --- The knob: a KY-040 rotary encoder on the 8-pin header --------------------------
// Not in config.h: the Arduino firmware keeps these in src/knob.cpp, and these are the same
// three values. A and SW are swapped relative to the header's natural order, to keep the
// ribbon flat.
#define BOARD_KNOB_A           GPIO_NUM_18   // encoder CLK
#define BOARD_KNOB_B           GPIO_NUM_17   // encoder DT
#define BOARD_KNOB_SW          GPIO_NUM_16   // push switch, active low
