#pragma once
#include <lvgl.h>
// The Weather app: MET Norway's forecast (yr.h). See yr_view.cpp. Registered with
// app_shell::addLazy(): built on the first visit, destroyed when evicted.
namespace yrview {
    lv_obj_t *build();
    void      destroy();
    void      onEnter();          // back to "now", with the latest forecast
    void      onTurn(int delta);  // into and along the wheel of hours, then days
    void      onPress();          // back to "now"
}
