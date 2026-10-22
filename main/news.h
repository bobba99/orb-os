// The Orb OS — Headlines from NRK and VG on ESP-IDF. See news.cpp. The screen calls it through
// src/intel_client.h's intel_fetch() and intel_brief_fetch(); Settings through these two.
#pragma once

int  host_news_sources();          // 0 NRK and VG, 1 NRK, 2 VG
void host_news_sources_set(int v);
