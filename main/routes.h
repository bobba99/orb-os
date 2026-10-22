// The Orb OS — flight routes on ESP-IDF. See routes.cpp.
#pragma once

namespace routes {

// Answer the radar's route_request(), if there is one. On the flight network task.
void service();

}  // namespace routes
