#pragma once
// The Weather app's icons (tools/make_weather_icons.py): plain shapes as LVGL 8-bit alpha
// images, painted in the theme's icon colour by whoever draws them.
#include <lvgl.h>

// The icon for one of MET Norway's symbol codes ("lightrain", "partlycloudy_night", ...),
// at 96 px (`big`) or 44 px. Never null: an unknown code draws as a cloud.
const lv_img_dsc_t *wx_icon(const char *symbolCode, bool big);
