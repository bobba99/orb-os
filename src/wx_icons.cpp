// MET Norway's symbol codes onto the Weather app's twelve icons. The codes are a condition,
// sometimes an intensity ("light", "heavy"), sometimes "showers" or "andthunder", and a
// "_day" / "_night" / "_polartwilight" variant; see api.met.no's weathericon list.
#include "wx_icons.h"
#include <string.h>

#define WX_DECLARE(name) extern "C" const lv_img_dsc_t wx_##name##_big, wx_##name##_small;
WX_DECLARE(sun) WX_DECLARE(moon) WX_DECLARE(fair_day) WX_DECLARE(fair_night) WX_DECLARE(cloud)
WX_DECLARE(fog) WX_DECLARE(rain_light) WX_DECLARE(rain) WX_DECLARE(rain_heavy) WX_DECLARE(sleet)
WX_DECLARE(snow) WX_DECLARE(thunder)

const lv_img_dsc_t *wx_icon(const char *code, bool big) {
#define PICK(name) return big ? &wx_##name##_big : &wx_##name##_small
    if (!code || !code[0]) PICK(cloud);
    const bool night = strstr(code, "_night") != nullptr;
    if (strstr(code, "thunder"))   PICK(thunder);
    if (strstr(code, "sleet"))     PICK(sleet);
    if (strstr(code, "snow"))      PICK(snow);
    if (strstr(code, "heavyrain")) PICK(rain_heavy);
    if (strstr(code, "lightrain")) PICK(rain_light);
    if (strstr(code, "rain"))      PICK(rain);
    if (!strncmp(code, "fog", 3))  PICK(fog);
    if (!strncmp(code, "clearsky", 8)) { if (night) PICK(moon); PICK(sun); }
    if (!strncmp(code, "fair", 4) || !strncmp(code, "partlycloudy", 12)) {
        if (night) PICK(fair_night);
        PICK(fair_day);
    }
    PICK(cloud);   // "cloudy", and anything MET adds later
#undef PICK
}
