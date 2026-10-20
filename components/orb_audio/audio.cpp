// The Orb OS — audio on ESP-IDF. Implements src/audio.h, for now as "no audio present".
//
// The ES8311 codec, its I2S stream and the playback task are not ported yet. Until they are,
// this is the state the shared code already handles for a board whose codec did not answer:
// audio_present() is false, so the clock's tick, the chime and the aircraft alerts are
// silent rather than faked, and every setting is still kept so a later port starts from the
// owner's levels. Every caller in src/ checks audio_present() or tolerates a no-op.
#include "audio.h"

static int  s_volume = 60, s_tick = 20, s_chime = 30, s_chimeIdx = 0;
static bool s_muted = false;

bool audio_begin() { return false; }
uint32_t audio_stack_free_bytes() { return 0; }
bool audio_present() { return false; }
void audio_set_volume(int pct) { s_volume = pct; }
void audio_set_muted(bool muted) { s_muted = muted; }
void audio_play(AudioCue) {}
void audio_play_pcm(const uint8_t *, size_t, bool, int) {}
void audio_set_tick_level(int pct) { s_tick = pct; }
int  audio_tick_level() { return s_tick; }
void audio_set_chime_level(int pct) { s_chime = pct; }
int  audio_chime_level() { return s_chime; }
void audio_release_pcm(const uint8_t *) {}   // nothing can be playing, so nothing to wait for
void audio_play_file(const char *, bool) {}
void audio_selftest() {}
int         audio_chime_count() { return 0; }
const char *audio_chime_name(int) { return ""; }
int         audio_chime_index() { return s_chimeIdx; }
void        audio_set_chime(int idx) { s_chimeIdx = idx; }
void        audio_preview_chime(int) {}
