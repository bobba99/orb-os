#include "yr.h"
#include <mutex>

// Written by the network task, read by LVGL: the same handoff as the headlines (intel.cpp).
static std::mutex s_mutex;
static YrForecast s_forecast = {};

void yr_store(const YrForecast &f) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_forecast = f;
}

bool yr_get(YrForecast &f) {
    std::lock_guard<std::mutex> lock(s_mutex);
    f = s_forecast;
    return f.valid;
}
