// The Orb OS — the Weather app: MET Norway's forecast for the Orb's location. Stage 8.
//
// Opens on "now": the place, the icon, the temperature, the wind and the rain in the next
// hour. Turning the knob moves into a wheel of the next twelve hours and then the next seven
// days, five rows at a time with the selected one in the middle; a press comes back to now.
// Rocking the knob leaves, as on every screen.
//
// Portable LVGL: the forecast arrives through yr.h, from main/yr.cpp on the device and from a
// fixed example in the simulator (--yrshot), so this file is the same in both. Colours are the
// theme's (weather_style.json: yrText, yrDim, yrAccent, yrIcon, bg; weather_plate.png behind),
// type is the built-in Montserrat, which carries æøå and ° since stage 6. MET's terms ask for
// "MET Norway" to be credited where the data is shown; the line under the forecast does.
#include "yr_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "plate_sprite.h"
#include "theme_style.h"
#include "wx_icons.h"
#include "yr.h"

extern bool host_wx_is_imperial();
extern bool host_location_name(char *out, size_t n);

namespace {

constexpr int ROWS = 5;            // wheel rows on screen; the middle one is the selected one
constexpr int ROW_DY = 64;

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_plate = nullptr;
// Now.
lv_obj_t *s_now = nullptr, *s_place = nullptr, *s_icon = nullptr, *s_temp = nullptr,
         *s_wind = nullptr, *s_rain = nullptr, *s_credit = nullptr;
// The wheel.
lv_obj_t *s_wheel = nullptr, *s_heading = nullptr;
lv_obj_t *s_rowIcon[ROWS] = {}, *s_rowText[ROWS] = {};
lv_timer_t *s_timer = nullptr;
plate_sprite::Plate s_plateArt { "weather_plate.png", "yr_plate" };

YrForecast s_f;                    // the copy being drawn
uint32_t   s_drawnMs = 0;          // its fetchedMs, to notice a new one
int        s_pos = -1;             // -1 now; 0.. hours then days

lv_color_t col(uint32_t hex) { return lv_color_hex(hex); }

// --- units --------------------------------------------------------------------------------

bool imperial() { return host_wx_is_imperial(); }
int  temp(float c) { return (int)lroundf(imperial() ? c * 9.0f / 5.0f + 32.0f : c); }

void fmt_rain(char *out, size_t n, float mm) {
    if (mm < 0.05f) { snprintf(out, n, "dry"); return; }
    if (imperial()) snprintf(out, n, "%.2f in", mm / 25.4f);
    else if (mm < 10.0f) snprintf(out, n, "%.1f mm", mm);
    else snprintf(out, n, "%.0f mm", mm);
}

const char *compass(int deg) {   // where the wind comes from
    static const char *P[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return P[((deg % 360 + 360) % 360 + 22) / 45 % 8];
}

const char *weekday(int w, bool today) {
    static const char *D[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    return today ? "Today" : D[(w % 7 + 7) % 7];
}

// --- drawing -------------------------------------------------------------------------------

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col(colour), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *icon(lv_obj_t *parent) {
    lv_obj_t *i = lv_img_create(parent);
    lv_obj_set_style_img_recolor(i, col(theme_style::weather().yrIcon), 0);
    lv_obj_set_style_img_recolor_opa(i, LV_OPA_COVER, 0);
    return i;
}

void draw_now() {
    char place[40] = "";
    host_location_name(place, sizeof(place));
    lv_label_set_text(s_place, place);
    if (!s_f.valid) {
        lv_img_set_src(s_icon, wx_icon("cloudy", true));
        lv_label_set_text(s_temp, "");
        lv_label_set_text(s_wind, "Getting the forecast...");
        lv_label_set_text(s_rain, "");
        lv_label_set_text(s_credit, "Data from MET Norway");
        return;
    }
    lv_img_set_src(s_icon, wx_icon(s_f.now.symbol, true));
    char b[64], r[24];
    snprintf(b, sizeof(b), "%d\xC2\xB0", temp(s_f.now.tempC));
    lv_label_set_text(s_temp, b);
    if (imperial()) snprintf(b, sizeof(b), "Wind %d mph %s", (int)lroundf(s_f.now.windMs * 2.237f), compass(s_f.now.windDeg));
    else            snprintf(b, sizeof(b), "Wind %d m/s %s", (int)lroundf(s_f.now.windMs), compass(s_f.now.windDeg));
    lv_label_set_text(s_wind, b);
    if (s_f.now.precipMm < 0.05f) {
        lv_label_set_text(s_rain, "Dry the next hour");
    } else {
        fmt_rain(r, sizeof(r), s_f.now.precipMm);
        snprintf(b, sizeof(b), "Rain %s the next hour", r);
        lv_label_set_text(s_rain, b);
    }
}

void draw_credit() {
    if (!s_f.valid) return;
    const uint32_t ageS = (lv_tick_get() - s_f.fetchedMs) / 1000;
    char b[48];
    if (ageS < 90) snprintf(b, sizeof(b), "MET Norway \xC2\xB7 just now");
    else if (ageS < 3600) snprintf(b, sizeof(b), "MET Norway \xC2\xB7 %lu min ago", (unsigned long)(ageS / 60));
    else snprintf(b, sizeof(b), "MET Norway \xC2\xB7 %lu h ago", (unsigned long)(ageS / 3600));
    lv_label_set_text(s_credit, b);
}

// Row k of the wheel (0.. hours then days) as one line of text, and its icon.
void row_text(int k, char *out, size_t n, const char **sym) {
    char r[24];
    if (k < s_f.nHours) {
        const YrHour &h = s_f.hours[k];
        fmt_rain(r, sizeof(r), h.precipMm);
        snprintf(out, n, "%02d:00   %d\xC2\xB0   %s", h.hour, temp(h.tempC), r);
        *sym = h.symbol;
    } else {
        const YrDay &d = s_f.days[k - s_f.nHours];
        fmt_rain(r, sizeof(r), d.precipMm);
        snprintf(out, n, "%s   %d\xC2\xB0 / %d\xC2\xB0   %s", weekday(d.wday, d.today), temp(d.minC), temp(d.maxC), r);
        *sym = d.symbol;
    }
}

void draw_wheel() {
    const int total = s_f.nHours + s_f.nDays;
    lv_label_set_text(s_heading, s_pos < s_f.nHours ? "Next hours" : "Next days");
    for (int i = 0; i < ROWS; ++i) {
        const int k = s_pos + i - ROWS / 2;
        const bool sel = i == ROWS / 2;
        if (k < 0 || k >= total) {
            lv_obj_add_flag(s_rowIcon[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_rowText[i], "");
            continue;
        }
        char t[64];
        const char *sym = "";
        row_text(k, t, sizeof(t), &sym);
        lv_label_set_text(s_rowText[i], t);
        lv_obj_set_style_text_font(s_rowText[i], sel ? &lv_font_montserrat_26 : &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_rowText[i], col(sel ? theme_style::weather().yrText : theme_style::weather().yrDim), 0);
        lv_obj_clear_flag(s_rowIcon[i], LV_OBJ_FLAG_HIDDEN);
        lv_img_set_src(s_rowIcon[i], wx_icon(sym, false));
        lv_obj_set_style_img_opa(s_rowIcon[i], sel ? LV_OPA_COVER : LV_OPA_60, 0);
        // A slight arc, so the wheel follows the round glass instead of a ruler.
        const int dy = (i - ROWS / 2) * ROW_DY;
        const int dx = (int)lroundf(fabsf((float)(i - ROWS / 2)) * 14.0f);
        lv_obj_align(s_rowIcon[i], LV_ALIGN_LEFT_MID, 58 + dx, dy);
        lv_obj_align(s_rowText[i], LV_ALIGN_LEFT_MID, 112 + dx, dy);
    }
}

void redraw() {
    if (!s_screen) return;
    const bool now = s_pos < 0 || !s_f.valid;
    if (now) {
        lv_obj_clear_flag(s_now, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wheel, LV_OBJ_FLAG_HIDDEN);
        draw_now();
        draw_credit();
    } else {
        lv_obj_add_flag(s_now, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wheel, LV_OBJ_FLAG_HIDDEN);
        draw_wheel();
    }
}

// Twice a second: a forecast that arrived since the last look, and the credit's age.
void tick(lv_timer_t *) {
    YrForecast f;
    yr_get(f);
    if (f.valid && f.fetchedMs != s_drawnMs) {
        s_f = f;
        s_drawnMs = f.fetchedMs;
        const int total = s_f.nHours + s_f.nDays;
        if (s_pos >= total) s_pos = total - 1;
        redraw();
    } else if (s_pos < 0) {
        draw_credit();
    }
}

lv_obj_t *group(lv_obj_t *parent) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, SCREEN_W, SCREEN_H);
    lv_obj_center(g);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

}  // namespace

lv_obj_t *yrview::build() {
    if (s_screen) return s_screen;
    const theme_style::Weather &st = theme_style::weather();
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, col(st.bg), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    if (const lv_img_dsc_t *art = plate_sprite::get(s_plateArt)) {
        s_plate = lv_img_create(s_screen);
        lv_img_set_src(s_plate, art);
        lv_obj_center(s_plate);
    }

    s_now = group(s_screen);
    s_place = label(s_now, &lv_font_montserrat_20, st.yrDim);
    lv_obj_align(s_place, LV_ALIGN_CENTER, 0, -150);
    s_icon = icon(s_now);
    lv_obj_align(s_icon, LV_ALIGN_CENTER, 0, -72);
    s_temp = label(s_now, &lv_font_montserrat_48, st.yrText);
    lv_obj_align(s_temp, LV_ALIGN_CENTER, 0, 20);
    s_wind = label(s_now, &lv_font_montserrat_20, st.yrText);
    lv_obj_align(s_wind, LV_ALIGN_CENTER, 0, 74);
    s_rain = label(s_now, &lv_font_montserrat_20, st.yrText);
    lv_obj_align(s_rain, LV_ALIGN_CENTER, 0, 104);
    s_credit = label(s_now, &lv_font_montserrat_14, st.yrDim);
    lv_obj_align(s_credit, LV_ALIGN_CENTER, 0, 160);

    s_wheel = group(s_screen);
    s_heading = label(s_wheel, &lv_font_montserrat_16, st.yrAccent);
    lv_obj_align(s_heading, LV_ALIGN_CENTER, 0, -186);
    for (int i = 0; i < ROWS; ++i) {
        s_rowIcon[i] = icon(s_wheel);
        s_rowText[i] = label(s_wheel, &lv_font_montserrat_20, st.yrDim);
        lv_obj_set_style_text_align(s_rowText[i], LV_TEXT_ALIGN_LEFT, 0);
    }

    yr_get(s_f);
    s_drawnMs = s_f.fetchedMs;
    s_pos = -1;
    redraw();
    s_timer = lv_timer_create(tick, 500, nullptr);
    return s_screen;
}

void yrview::destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_screen) lv_obj_del(s_screen);
    plate_sprite::release(s_plateArt);
    s_screen = s_plate = s_now = s_place = s_icon = s_temp = s_wind = s_rain = s_credit = nullptr;
    s_wheel = s_heading = nullptr;
    for (int i = 0; i < ROWS; ++i) s_rowIcon[i] = s_rowText[i] = nullptr;
}

void yrview::onEnter() {
    s_pos = -1;
    yr_get(s_f);
    s_drawnMs = s_f.fetchedMs;
    redraw();
}

void yrview::onTurn(int delta) {
    if (!s_screen || !s_f.valid) return;
    const int total = s_f.nHours + s_f.nDays;
    s_pos += delta > 0 ? 1 : -1;
    if (s_pos < -1) s_pos = -1;
    if (s_pos >= total) s_pos = total - 1;
    redraw();
}

void yrview::onPress() {
    if (!s_screen) return;
    s_pos = -1;
    redraw();
}
