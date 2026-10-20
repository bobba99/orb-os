// The Orb OS — the microSD card on ESP-IDF. Implements src/sdcard.h.
//
// A port of src/sdcard.cpp: the same SPI wiring and the same speed negotiation, on ESP-IDF's
// SDSPI driver and FAT VFS. Once mounted the card is ordinary POSIX files under ORB_SD_ROOT
// (orb_platform.h), which is how the shared code's non-Arduino paths read it.
#include "sdcard.h"
#include "board.h"
#include "orb_platform.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_protocol_defs.h"   // SD_OCR_SDHC_CAP
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "sd";

// The display has SPI2 (its QSPI bus), so the card takes SPI3, as HSPI was on Arduino.
static constexpr spi_host_device_t SD_SPI_HOST = SPI3_HOST;

// Fall back rather than refuse, exactly as src/sdcard.cpp does and for its reason: 20 MHz is
// what a modern card manages and what Spy Cam needs, but an SDSC card (2 GB and under) can
// power up, click, and then fail to mount at 20 with no hint why. Overcore lost a day to that
// on 2026-09-29. So the speed is negotiated instead of assumed.
static constexpr int SD_SPI_TRY_KHZ[] = { 20000, 10000, 4000 };

static bool          s_mounted   = false;
static uint64_t      s_sizeBytes = 0;
static sdmmc_card_t *s_card      = nullptr;

bool sdcard::begin() {
    spi_bus_config_t bus = {};
    bus.mosi_io_num     = BOARD_SD_MOSI;
    bus.miso_io_num     = BOARD_SD_MISO;
    bus.sclk_io_num     = BOARD_SD_SCK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus for the card: %s", esp_err_to_name(err));
        return false;
    }

    esp_vfs_fat_sdmmc_mount_config_t mountCfg = {};
    // NEVER format. This call can be told to wipe a card it cannot mount, and that card holds
    // somebody's themes. A card this firmware cannot read is reported, never erased.
    mountCfg.format_if_mount_failed = false;
    mountCfg.max_files = 8;
    mountCfg.allocation_unit_size = 16 * 1024;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_SD_CS;
    slot.host_id = SD_SPI_HOST;

    int khz = 0;
    for (int tryKhz : SD_SPI_TRY_KHZ) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SD_SPI_HOST;
        host.max_freq_khz = tryKhz;
        err = esp_vfs_fat_sdspi_mount(ORB_SD_ROOT, &host, &slot, &mountCfg, &s_card);
        if (err == ESP_OK) { khz = tryKhz; break; }
    }
    if (!khz) {
        ESP_LOGW(TAG, "no card mounted (tried 20, 10 and 4 MHz; last error %s)", esp_err_to_name(err));
        spi_bus_free(SD_SPI_HOST);
        return false;
    }
    if (khz != SD_SPI_TRY_KHZ[0])
        ESP_LOGW(TAG, "card would not run at %d MHz, using %d MHz instead",
                 SD_SPI_TRY_KHZ[0] / 1000, khz / 1000);

    s_sizeBytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    const char *type = (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC" : "SDSC";
    // Same wording as the Arduino log, so the two boot logs compare line for line.
    printf("[sd] %s card mounted, %.2f GB\n", type, s_sizeBytes / (1024.0 * 1024.0 * 1024.0));
    s_mounted = true;
    return true;
}

bool sdcard::mounted() { return s_mounted; }
uint64_t sdcard::sizeBytes() { return s_sizeBytes; }
