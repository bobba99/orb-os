// The Orb OS — WiFi on ESP-IDF: an event-driven state machine over several saved networks.
//
// Stage 3 of REWRITE_PLAN.md. One manager task owns every WiFi decision; ESP-IDF's WiFi and
// IP events, timers and requests from the UI reach it through one queue.
//
//   IDLE -> CONNECTING -> CONNECTED
//              |   ^         |  (link lost: straight back to CONNECTING)
//              v   |         v
//            BACKOFF  (1 s doubling to 30 s, then a new round)
//   PROVISIONING: no saved network has answered for ORB_WIFI_SETUP_AFTER_S (or there is none):
//                 "The Orb Setup" is up beside the rounds, which carry on every 30 s.
//
// A round scans, then tries the saved networks that are visible, strongest first, then any
// that were not seen (a hidden network does not answer a scan), each for up to 20 s.
//
// Two promises, kept here for every caller:
//  1. The WiFi driver's own stored network is read once, to seed the list, and never written:
//     the driver runs on RAM-only storage. The list lives in orb_prefs ("capsuleradar").
//  2. Trying a network never costs the owner the ones they have. A candidate is saved only
//     once it has actually produced an IP address; until then the list is untouched, and a
//     failed attempt returns the Orb to its own networks. Nothing here ever reboots.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace orb_wifi {

enum class State : uint8_t { Idle, Connecting, Connected, Backoff, Provisioning };
const char *stateName(State s);

// Start the manager. False if WiFi could not be started at all.
bool begin();

State state();
bool  connected();
int   rssi();                               // dBm, or -127 when not connected
void  currentSsid(char *out, size_t n);     // the joined network, "" if none
// The network to name on screen: the joined one, else the first saved, else "".
void  savedSsid(char *out, size_t n);
bool  timeSynced();                         // SNTP has set the clock at least once

// --- the saved list, in priority order (the last network that worked first) ---
constexpr int MAX_SAVED = 8;
int  savedCount();
bool savedAt(int i, char *ssid, size_t n);
bool forget(const char *ssid);              // removed and saved; a joined network is left

// --- trying a network: the setup page and Settings ---
// Join `ssid` now. Saved, at the front of the list, only once it produces an IP address; on
// a wrong password, no such network, or 20 s without an address, the Orb goes back to its
// own networks and the list is as it was. False if a try is already running.
bool tryNetwork(const char *ssid, const char *pass);
enum class TryResult : uint8_t { None, Trying, Joined, Failed };
TryResult tryResult();

// --- scanning for a picker ---
void startScan();
// -1 still scanning, -2 failed, else the number written. One entry per name, strongest kept.
int  scanResults(char names[][33], int8_t *rssi, bool *isOpen, int maxN);

}  // namespace orb_wifi
