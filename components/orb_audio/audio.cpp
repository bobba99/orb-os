// The Orb OS — audio on ESP-IDF: the ES8311 codec and the speaker. Implements src/audio.h.
//
// A port of src/audio.cpp, the Arduino firmware's driver, with its behaviour kept: the same
// ES8311 register sequence (Espressif's esp_codec_dev for this board: DAC playback, I2S slave,
// 16 kHz, 16-bit), the same cues and sounds, the same "the newest request wins" interruption,
// the same volume curves, and the hour's chime that nothing may cut short. What changes is the
// plumbing underneath:
//
//  - I2C through orb_i2c (ESP-IDF's i2c_master), not Wire.
//  - I2S through the current i2s_std driver, not the legacy one.
//  - The codec's analog supply. The AXP2101 PMIC's ALDO1 feeds the ES8311; Arduino switched it
//    on from battery.cpp before audio_begin(). Here audio_begin() does it, writing only ALDO1's
//    own enable bit and voltage field (XPowersLib's registers, 0x90 bit 0 and 0x92 bits 4..0),
//    so no other rail is touched.
//  - Files on the card are opened through the VFS at /sdcard. FATFS serialises its own access,
//    so the Arduino SD library's lock (theme_sd::lock) has no counterpart here.
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "chime_westminster.h"
#include "config.h"          // PIN_I2S_*, PIN_AUDIO_PA
#include "orb_i2c.h"
#include "orb_platform.h"    // ORB_SD_ROOT, delay()

namespace {

constexpr uint8_t ES8311_ADDR = 0x18;
constexpr uint8_t AXP2101_ADDR = 0x34;
constexpr int     SR = 16000;                       // a beep and a chime; pitch-tolerant
constexpr size_t  S_BUF_LEN = SR / 2 * 2;           // up to 500 ms, stereo interleaved
constexpr size_t  WRITE_CHUNK = SR / 32 * 2;        // ~30 ms: the abort latency (src/audio.cpp)

i2s_chan_handle_t s_tx = nullptr;
i2c_master_dev_handle_t s_es = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_sem = nullptr;
bool s_ok = false;
int16_t *s_buf = nullptr;                           // PSRAM: internal RAM is for TLS

volatile int   s_vol = 60, s_tickPct = 20, s_chimePct = 30;
volatile float s_trimGain = 1.0f;
volatile bool  s_muted = false;
volatile int   s_cue = -1;
volatile uint32_t s_gen = 0;                        // bumped by every request; playback checks it
volatile bool  s_sustained = false;                 // the hour's chime: never cut short
volatile int   s_chimeIdx = 0, s_previewIdx = 0;
const uint8_t *s_pcm = nullptr;                     // a theme's sound, borrowed (theme_audio owns it)
size_t         s_pcmLen = 0;
const uint8_t *volatile s_playingPcm = nullptr;     // which borrowed buffer is being read
char s_filePath[128] = { 0 };

struct ChimeInfo { const char *name; const uint8_t *pcm; size_t bytes; };
const ChimeInfo CHIMES[] = { { "Westminster", CHIME_WESTMINSTER_PCM, CHIME_WESTMINSTER_BYTES } };
constexpr int CHIME_COUNT = (int)(sizeof(CHIMES) / sizeof(CHIMES[0]));

int clamp_pct(int p) { return p < 0 ? 0 : (p > 100 ? 100 : p); }

// --- registers -------------------------------------------------------------------------------

bool reg_write(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val) {
    const uint8_t b[2] = { reg, val };
    return d && i2c_master_transmit(d, b, 2, 100) == ESP_OK;
}

bool reg_read(i2c_master_dev_handle_t d, uint8_t reg, uint8_t &val) {
    return d && i2c_master_transmit_receive(d, &reg, 1, &val, 1, 100) == ESP_OK;
}

void es_write(uint8_t reg, uint8_t val) { reg_write(s_es, reg, val); }
uint8_t es_read(uint8_t reg) { uint8_t v = 0; reg_read(s_es, reg, v); return v; }
void es_update(uint8_t reg, uint8_t andmask, uint8_t ormask) {
    es_write(reg, (uint8_t)((es_read(reg) & andmask) | ormask));
}

// The ES8311's analog supply: AXP2101 ALDO1 at 3.3 V, as battery_enable_codec_rail() does.
bool codec_rail_on() {
    if (!orb_i2c::present(AXP2101_ADDR)) { printf("[audio] AXP2101 not found; codec rail left as it is\n"); return false; }
    i2c_master_dev_handle_t pmu = orb_i2c::device(AXP2101_ADDR);
    uint8_t vol = 0, on = 0;
    if (!reg_read(pmu, 0x92, vol) || !reg_read(pmu, 0x90, on)) return false;
    const uint8_t want = (uint8_t)((vol & 0xE0) | ((3300 - 500) / 100));   // 3.3 V
    if (vol != want && !reg_write(pmu, 0x92, want)) return false;
    if (!(on & 0x01) && !reg_write(pmu, 0x90, (uint8_t)(on | 0x01))) return false;
    printf("[audio] ALDO1 (codec AVDD) on at 3.3 V%s\n", (on & 0x01) ? " (already was)" : "");
    return true;
}

// src/audio.cpp es8311_init(), register for register.
void es8311_init() {
    es_write(0x0D, 0xFA);
    es_write(0x44, 0x08);
    es_write(0x44, 0x08);                 // written twice: the ES8311's first-write quirk
    es_write(0x01, 0x30);
    es_write(0x02, 0x00);
    es_write(0x03, 0x10);
    es_write(0x16, 0x24);
    es_write(0x04, 0x10);
    es_write(0x05, 0x00);
    es_write(0x0B, 0x00);
    es_write(0x0C, 0x00);
    es_write(0x10, 0x1F);
    es_write(0x11, 0x7F);
    es_write(0x00, 0x80);
    es_write(0x00, 0x80);
    es_write(0x01, 0xBF);
    es_update(0x06, (uint8_t)~0x20, 0x00);
    es_write(0x13, 0x10);
    es_write(0x1B, 0x0A);
    es_write(0x1C, 0x6A);
    es_write(0x44, 0x58);
    es_write(0x02, 0x18);
    es_write(0x05, 0x00);
    es_write(0x03, 0x10);
    es_write(0x04, 0x20);
    es_update(0x07, 0xC0, 0x00);
    es_write(0x08, 0xFF);
    es_update(0x06, 0xE0, 0x03);
    es_write(0x09, 0x0C);
    es_write(0x0A, 0x0C);
    es_write(0x00, 0x80);
    es_write(0x01, 0xBF);
    es_write(0x09, 0x0C);
    es_write(0x0A, 0x0C);
    es_write(0x17, 0xBF);
    es_write(0x0E, 0x02);
    es_write(0x12, 0x00);
    es_write(0x14, 0x1A);
    es_write(0x0D, 0x01);
    es_write(0x15, 0x40);
    es_write(0x37, 0x08);
    es_write(0x45, 0x00);
    es_write(0x32, 0xBF);                 // DAC volume about 0 dB
    es_update(0x31, 0x9F, 0x00);          // unmute
}

// Master, TX only, 16 kHz 16-bit stereo, MCLK = 256 fs, the board's pins. Filled field by field:
// the ESP-IDF initialiser macros are C designated initialisers in an order C++ does not accept.
bool i2s_setup() {
    i2s_chan_config_t chan = {};
    chan.id = I2S_NUM_0;
    chan.role = I2S_ROLE_MASTER;
    chan.dma_desc_num = 4;
    chan.dma_frame_num = 256;
    chan.auto_clear = true;               // silence, not the last buffer again, when starved
    if (i2s_new_channel(&chan, &s_tx, nullptr) != ESP_OK) return false;
    i2s_std_config_t std = {};
    std.clk_cfg.sample_rate_hz = SR;
    std.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    std.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std.slot_cfg.ws_width = 16;
    std.slot_cfg.ws_pol = false;
    std.slot_cfg.bit_shift = true;        // Philips (standard I2S)
    std.slot_cfg.left_align = true;
    std.slot_cfg.big_endian = false;
    std.slot_cfg.bit_order_lsb = false;
    std.gpio_cfg.mclk = (gpio_num_t)PIN_I2S_MCLK;
    std.gpio_cfg.bclk = (gpio_num_t)PIN_I2S_BCLK;
    std.gpio_cfg.ws = (gpio_num_t)PIN_I2S_LRCLK;
    std.gpio_cfg.dout = (gpio_num_t)PIN_I2S_DOUT;
    std.gpio_cfg.din = I2S_GPIO_UNUSED;
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }
    return true;
}

// --- playback --------------------------------------------------------------------------------

void out(const int16_t *samples, size_t n) {
    size_t written = 0;
    i2s_channel_write(s_tx, samples, n * sizeof(int16_t), &written, portMAX_DELAY);
}

// Drop what the DMA still holds, so a sound that was stopped stops now and not in 60 ms.
void flush() {
    i2s_channel_disable(s_tx);
    i2s_channel_enable(s_tx);
}

bool superseded(uint32_t myGen) {
    if (s_gen == myGen) return false;
    flush();
    return true;
}

size_t gen_beep(int16_t *buf, size_t cap, float freq, int ms, float amp) {
    const size_t n = (size_t)((long)SR * ms / 1000);
    const size_t fade = SR / 200;                    // ~5 ms ramps: no clicks
    size_t i = 0;
    for (; i < n && (i * 2 + 1) < cap; ++i) {
        float env = 1.0f;
        if (i < fade) env = (float)i / fade;
        else if (i > n - fade) env = (float)(n - i) / fade;
        const int16_t s = (int16_t)(amp * env * sinf(2.0f * (float)M_PI * freq * i / SR));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    return i * 2;
}

void play_pcm(const uint8_t *data, size_t bytes) {
    if (!s_buf || !data || bytes < 2) return;
    const uint32_t myGen = s_gen;
    const float g = (s_vol / 100.0f) * s_trimGain;
    const int16_t *src = (const int16_t *)data;
    const size_t total = bytes / 2;
    for (size_t i = 0; i < total; ) {
        if (s_gen != myGen) { flush(); return; }
        size_t chunk = total - i < WRITE_CHUNK ? total - i : WRITE_CHUNK;
        for (size_t k = 0; k < chunk; ++k) s_buf[k] = (int16_t)(src[i + k] * g);
        out(s_buf, chunk);
        i += chunk;
    }
}

// A file from the card, in the same short pieces, so an abort is as quick and a long chime
// costs no more memory than a short one. Paths come as the Arduino firmware wrote them
// ("/themes/<slug>/..."), relative to the card.
void play_file(const char *path, bool sustained) {
    if (!s_buf || !path || !*path) return;
    char full[160];
    snprintf(full, sizeof(full), "%s%s", strncmp(path, ORB_SD_ROOT, strlen(ORB_SD_ROOT)) ? ORB_SD_ROOT : "", path);
    FILE *f = fopen(full, "rb");
    if (!f) { printf("[audio] cannot open %s\n", full); return; }
    const uint32_t myGen = s_gen;
    const float g = (s_vol / 100.0f) * (sustained ? s_chimePct / 100.0f : 1.0f);
    s_sustained = sustained;
    for (;;) {
        if (s_gen != myGen) { flush(); break; }
        const size_t got = fread(s_buf, 1, WRITE_CHUNK * sizeof(int16_t), f);
        if (got < 2) break;
        const size_t n = got / sizeof(int16_t);
        for (size_t k = 0; k < n; ++k) s_buf[k] = (int16_t)(s_buf[k] * g);
        out(s_buf, n);
    }
    s_sustained = false;
    fclose(f);
}

// src/audio.cpp play_cue(): 2 self-test, 4 chime preview, 6/7 a theme's sound, 8/9 a file
// (the hour's chime, or its preview), and the named cues.
void play_cue(int cue) {
    const uint32_t myGen = s_gen;
    if (!s_ok || !s_buf || (s_muted && cue != 2 && cue != 4 && cue != 7 && cue != 9) || s_vol <= 0) return;
    const float amp = (s_vol / 100.0f) * 17000.0f;
    gpio_set_level((gpio_num_t)PIN_AUDIO_PA, 1);      // the speaker amplifier, only while sounding
    vTaskDelay(pdMS_TO_TICKS(8));
    if (cue == 2) {
        const size_t ns = gen_beep(s_buf, S_BUF_LEN, 1000.0f, 480, amp);
        for (int k = 0; k < 4 && !superseded(myGen); ++k) out(s_buf, ns);
    } else if (cue == AUDIO_ALERT) {
        for (int k = 0; k < 2 && !superseded(myGen); ++k) {
            out(s_buf, gen_beep(s_buf, S_BUF_LEN, 1320.0f, 80, amp));
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    } else if (cue == AUDIO_CHIME) {
        const int idx = s_chimeIdx < 0 ? 0 : (s_chimeIdx >= CHIME_COUNT ? CHIME_COUNT - 1 : s_chimeIdx);
        s_sustained = true;
        play_pcm(CHIMES[idx].pcm, CHIMES[idx].bytes);
        s_sustained = false;
    } else if (cue == 4) {
        const int idx = s_previewIdx < 0 ? 0 : (s_previewIdx >= CHIME_COUNT ? CHIME_COUNT - 1 : s_previewIdx);
        play_pcm(CHIMES[idx].pcm, CHIMES[idx].bytes);
    } else if (cue == 8 || cue == 9) {
        play_file(s_filePath, cue == 8);
    } else if (cue == 6 || cue == 7) {
        if (s_pcm && s_pcmLen >= 2) {
            s_playingPcm = s_pcm;
            play_pcm(s_pcm, s_pcmLen);
            s_playingPcm = nullptr;
        }
    } else if (cue == AUDIO_WIND) {
        out(s_buf, gen_beep(s_buf, S_BUF_LEN, 2400.0f, 9, amp * 0.45f));   // a dry tick, per detent
    } else {
        out(s_buf, gen_beep(s_buf, S_BUF_LEN, 880.0f, 160, amp));
    }
    vTaskDelay(pdMS_TO_TICKS(90));                    // let the tail clock out before the amp goes
    gpio_set_level((gpio_num_t)PIN_AUDIO_PA, 0);
}

void audio_task(void *) {
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;
        while (xSemaphoreTake(s_sem, 0) == pdTRUE) {}   // only the newest request is played
        play_cue(s_cue);
    }
}

void request(int cue) {
    s_cue = cue;
    ++s_gen;
    if (s_sem) xSemaphoreGive(s_sem);
}

}  // namespace

bool audio_begin() {
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << PIN_AUDIO_PA;
    pa.mode = GPIO_MODE_OUTPUT;
    gpio_config(&pa);
    gpio_set_level((gpio_num_t)PIN_AUDIO_PA, 0);

    codec_rail_on();
    vTaskDelay(pdMS_TO_TICKS(10));                  // the rail settles before the codec is asked
    if (!orb_i2c::present(ES8311_ADDR)) { printf("[audio] ES8311 not found\n"); return false; }
    s_es = orb_i2c::device(ES8311_ADDR);
    const uint8_t id1 = es_read(0xFD), id2 = es_read(0xFE);   // 0x83 0x11
    if (id1 != 0x83) {
        printf("[audio] ES8311 not found (id=0x%02X 0x%02X)\n", id1, id2);
        return false;
    }
    es8311_init();
    if (!i2s_setup()) { printf("[audio] I2S init failed\n"); return false; }
    s_buf = (int16_t *)heap_caps_malloc(S_BUF_LEN * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) { printf("[audio] no memory for the sample buffer\n"); return false; }
    s_sem = xSemaphoreCreateBinary();
    // 8 KB as on Arduino: chimes stream off the card, and the VFS and FATFS need room.
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 1, &s_task, 0);
    s_ok = true;
    printf("[audio] ES8311 ready (id 0x%02X 0x%02X)\n", id1, id2);
    return true;
}

bool audio_present() { return s_ok; }
uint32_t audio_stack_free_bytes() { return s_task ? uxTaskGetStackHighWaterMark(s_task) : 0; }
void audio_set_volume(int pct) { s_vol = clamp_pct(pct); }
void audio_set_muted(bool m) { s_muted = m; }
void audio_set_tick_level(int pct) { s_tickPct = clamp_pct(pct); }
int  audio_tick_level() { return s_tickPct; }
void audio_set_chime_level(int pct) { s_chimePct = clamp_pct(pct); }
int  audio_chime_level() { return s_chimePct; }

void audio_play(AudioCue cue) {
    if (!s_ok || s_muted || s_sustained) return;    // never cut the hour short
    request((int)cue);
}

void audio_play_pcm(const uint8_t *pcm, size_t bytes, bool ignoreMute, int trimPct) {
    if (!s_ok || (s_muted && !ignoreMute) || !pcm || bytes < 2) return;
    if (s_sustained && !ignoreMute) return;
    if (trimPct <= 0) return;
    const float t = (trimPct > 100 ? 100 : trimPct) / 100.0f;
    s_trimGain = t * t;                             // squared: hearing is not linear (src/audio.cpp)
    s_pcm = pcm;
    s_pcmLen = bytes;
    request(ignoreMute ? 7 : 6);
}

void audio_release_pcm(const uint8_t *pcm) {
    if (!pcm) return;
    ++s_gen;
    for (int i = 0; i < 100 && s_playingPcm == pcm; ++i) vTaskDelay(pdMS_TO_TICKS(2));
}

void audio_play_file(const char *path, bool preview) {
    if (!s_ok || (s_muted && !preview) || !path || !*path) return;
    if (s_sustained && !preview) return;
    snprintf(s_filePath, sizeof(s_filePath), "%s", path);
    request(preview ? 9 : 8);
}

void audio_selftest() { if (s_ok) request(2); }
int  audio_chime_count() { return CHIME_COUNT; }
const char *audio_chime_name(int idx) { return (idx >= 0 && idx < CHIME_COUNT) ? CHIMES[idx].name : ""; }
int  audio_chime_index() { return s_chimeIdx; }
void audio_set_chime(int idx) { s_chimeIdx = idx < 0 ? 0 : (idx >= CHIME_COUNT ? CHIME_COUNT - 1 : idx); }
void audio_preview_chime(int idx) {
    if (!s_ok) return;
    s_previewIdx = idx < 0 ? 0 : (idx >= CHIME_COUNT ? CHIME_COUNT - 1 : idx);
    request(4);
}
