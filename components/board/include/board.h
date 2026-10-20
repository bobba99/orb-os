// The Orb OS — the Waveshare ESP32-S3-Touch-AMOLED-1.75, as this firmware wires it.
//
// Every GPIO the firmware touches is named here and nowhere else. The values are the
// Arduino build's (src/config.h, src/knob.cpp), which run on real Orbs, cross-checked
// against Waveshare's own board package (waveshare/esp32_s3_touch_amoled_1_75 3.0.1):
// every pin below agrees with it. Do not add a pin by guessing; take it from one of those.
#pragma once
#include "driver/gpio.h"

// --- Display: CO5300 AMOLED, 466 x 466, QSPI --------------------------------------
#define BOARD_LCD_H_RES        466
#define BOARD_LCD_V_RES        466
#define BOARD_LCD_CS           GPIO_NUM_12
#define BOARD_LCD_PCLK         GPIO_NUM_38
#define BOARD_LCD_D0           GPIO_NUM_4
#define BOARD_LCD_D1           GPIO_NUM_5
#define BOARD_LCD_D2           GPIO_NUM_6
#define BOARD_LCD_D3           GPIO_NUM_7
#define BOARD_LCD_RST          GPIO_NUM_39
#define BOARD_LCD_TE           GPIO_NUM_13   // tearing-effect output; not used yet
// The panel's first visible column is column 6 of the controller's memory.
#define BOARD_LCD_COL_OFFSET   6
#define BOARD_LCD_ROW_OFFSET   0

// --- Shared I2C: touch, IMU, RTC, PMIC, codec --------------------------------------
#define BOARD_I2C_SDA          GPIO_NUM_15
#define BOARD_I2C_SCL          GPIO_NUM_14

// --- Touch (CST9217), present but unused: the Orb is knob-only ---------------------
#define BOARD_TP_INT           GPIO_NUM_11
#define BOARD_TP_RST           GPIO_NUM_40

// --- Audio: ES8311 over I2S, speaker amp -------------------------------------------
#define BOARD_I2S_MCLK         GPIO_NUM_42
#define BOARD_I2S_BCLK         GPIO_NUM_9
#define BOARD_I2S_WS           GPIO_NUM_45
#define BOARD_I2S_DOUT         GPIO_NUM_8
#define BOARD_I2S_DIN          GPIO_NUM_10
#define BOARD_AUDIO_PA         GPIO_NUM_46

// --- The knob: a KY-040 rotary encoder on the 8-pin header --------------------------
// A and SW are swapped relative to the header's natural order, to keep the ribbon flat.
#define BOARD_KNOB_A           GPIO_NUM_18   // encoder CLK
#define BOARD_KNOB_B           GPIO_NUM_17   // encoder DT
#define BOARD_KNOB_SW          GPIO_NUM_16   // push switch, active low
