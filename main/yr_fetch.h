// The Orb OS — the Weather app's forecast from MET Norway, on ESP-IDF. See yr.cpp.
#pragma once

// On the network task, every pass: fetches when the forecast is due (ten minutes, or the
// location moved) and hands the result to the screen through src/yr.h. Returns at once
// otherwise.
void yr_service();
