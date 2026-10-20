// The Orb OS — the display and knob bring-up page (CONFIG_ORB_BRINGUP_PAGE).
//
// Exercises the panel and the knob and nothing else, so a flash of a new display or knob
// driver answers the two questions that come before any app: does the panel draw correctly
// (orientation, colour order, the 6-pixel column gap, the 2-pixel window rule), and does the
// knob count, press and rock the way the Arduino firmware's does.
//
// NVS and the SD card are deliberately not opened. Nothing here needs them, and leaving them
// alone means this page cannot change anything the firmware saved.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

#include "board.h"
#include "display.h"
#include "display_idf.h"
#include "knob.h"

static const char *TAG = "bringup";

static constexpr int     ARC_DETENTS        = 24;    // one lap of the arc

static lv_obj_t *s_arc     = nullptr;
static lv_obj_t *s_count   = nullptr;
static lv_obj_t *s_presses = nullptr;
static lv_obj_t *s_rock    = nullptr;

static void build_bringup_screen() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Touching the round edge on all sides is the check for the column gap: an offset panel
    // clips the ring on one side and leaves a gap on the other.
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, BOARD_LCD_H_RES - 8, BOARD_LCD_V_RES - 8);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, ARC_DETENTS);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0xE05A3A), LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "The Orb OS\nESP-IDF bring-up");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    s_count = lv_label_create(scr);
    lv_label_set_text(s_count, "0");
    lv_obj_set_style_text_font(s_count, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_count, lv_color_white(), 0);
    lv_obj_align(s_count, LV_ALIGN_CENTER, 0, -10);

    s_presses = lv_label_create(scr);
    lv_label_set_text(s_presses, "turn the knob, push it");
    lv_obj_set_style_text_font(s_presses, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_presses, lv_color_hex(0xA0A0A0), 0);
    lv_obj_align(s_presses, LV_ALIGN_CENTER, 0, 50);

    s_rock = lv_label_create(scr);
    lv_label_set_text(s_rock, "");
    lv_obj_set_style_text_font(s_rock, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_rock, lv_color_hex(0x7DDB8A), 0);
    lv_obj_align(s_rock, LV_ALIGN_CENTER, 0, 90);

    // Red, green and blue swatches: wrong colour order or byte swap shows up here at once.
    static const uint32_t kSwatch[] = {0xFF0000, 0x00FF00, 0x0000FF};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *sw = lv_obj_create(scr);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 40, 40);
        lv_obj_set_style_bg_color(sw, lv_color_hex(kSwatch[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_align(sw, LV_ALIGN_BOTTOM_MID, (i - 1) * 56, -80);
    }
}

static void input_task(void *) {
    int32_t  detents = 0;
    uint32_t presses = 0;
    uint32_t lastRock = 0;
    for (;;) {
        // The lock is held for the whole pass and let go only for the sleep at the bottom,
        // which is when esp_lvgl_port's task renders. See display_idf.h.
        display_idf::lock();
        knob::poll();
        if (knob::takeLongPress()) {
            ESP_LOGI(TAG, "knob long-press -> reboot");
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_restart();
        }
        const int32_t delta = knob::takeDelta();
        const bool pressed = knob::takePress();
        const uint32_t rock = knob::lastRockMs();
        const bool rocked = rock != 0 && rock != lastRock;
        if (delta || pressed || rocked) {
            detents += delta;
            if (pressed) ++presses;
            if (rocked) lastRock = rock;
            {
                char buf[48];
                snprintf(buf, sizeof(buf), "%ld", (long)detents);
                lv_label_set_text(s_count, buf);
                const int32_t lap = ((detents % ARC_DETENTS) + ARC_DETENTS) % ARC_DETENTS;
                lv_arc_set_value(s_arc, lap);
                if (pressed) {
                    snprintf(buf, sizeof(buf), "pressed %lu", (unsigned long)presses);
                    lv_label_set_text(s_presses, buf);
                }
                if (rocked) {
                    snprintf(buf, sizeof(buf), "rock, %lu ms", (unsigned long)knob::lastRockGapMs());
                    lv_label_set_text(s_rock, buf);
                }
            }
        }
        display_idf::unlock();
        vTaskDelay(pdMS_TO_TICKS(5));   // the Arduino loop's own 5 ms pace
    }
}

// Called from app_main() when CONFIG_ORB_BRINGUP_PAGE is set; returns once its task runs.
void bringup_run() {
    ESP_LOGI(TAG, "The Orb OS, ESP-IDF bring-up page");
    ESP_LOGI(TAG, "PSRAM free %u KB, internal free %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    if (!display::begin()) {
        ESP_LOGE(TAG, "display did not come up; knob still starts so the log shows it");
    } else {
        display::setBrightness(BRIGHTNESS_DEFAULT);
        display_idf::lock();
        build_bringup_screen();
        display_idf::unlock();
    }

    knob::begin();
    // Core 1 with LVGL, as the Arduino loop() ran: input and rendering share a core, and
    // core 0 stays free for the network stack when it arrives.
    xTaskCreatePinnedToCore(input_task, "input", 4096, nullptr, 5, nullptr, 1);

    ESP_LOGI(TAG, "internal free after bring-up %u KB, largest block %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}
