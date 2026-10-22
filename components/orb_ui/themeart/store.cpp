// The Orb OS — the theme art store on ESP-IDF: theme_art.h's API over the `themeart` flash
// partition. Stage 5 of REWRITE_PLAN.md.
//
// src/theme_art.cpp is the Arduino firmware's store and is left as it is. This one keeps its
// idea (raw pixels in memory-mapped flash, drawn with no decode and no PSRAM) and changes how
// the partition is used:
//
//  - Content-addressed. Each blob carries a hash of its bytes; an asset whose bytes are
//    already stored, by this theme or another, points at the existing blob instead of
//    writing a second copy. A theme that uses one glass on three screens stores it once.
//  - Indexed formats (theme_art.h): the baker (bake.cpp) stores a picture of 256 colours or
//    fewer as palette + indices, which LVGL draws straight from flash.
//  - Space is reused. A blob goes in the first gap that holds it, so the room a removed
//    theme leaves is used again instead of the partition only ever filling.
//  - Reclaiming (reclaim_missing): art belonging to a theme no longer on the card is dropped
//    at boot.
//
// The layout is version 9. A version-8 partition (the Arduino store's) reads as empty here,
// so the first boot re-bakes from the card, and the Arduino store does the same in reverse.
#include "theme_art.h"

#include <stdio.h>
#include <string.h>
#include "esp_partition.h"
#include "theme_select.h"

namespace theme_art {
namespace {

constexpr uint32_t MAGIC       = 0x4F524254;   // 'ORBT'
constexpr uint32_t VERSION     = 9;
constexpr size_t   INDEX_BYTES = 8192;          // two 4 KB sectors
constexpr size_t   SECTOR      = 4096;

struct Header {
    uint32_t magic, version, count, used;   // used: bytes of distinct blobs, padded to sectors
};

struct Entry {                 // 64 bytes, as in version 8
    uint32_t slugHash;         // FNV-1a over the whole slug (see src/theme_art.cpp for why)
    char     name[24];
    uint32_t offset;           // from partition start; several entries may share one
    uint32_t len;
    uint16_t w, h;
    uint8_t  fmt;
    uint8_t  pad[3];
    uint32_t manifest;         // theme_style::assetsFingerprint() for this entry's theme
    uint32_t contentHash;      // FNV-1a over the blob's bytes: how a shared blob is found
    char     slugPrefix[12];   // logs only
};
static_assert(sizeof(Entry) == 64, "Entry must stay 64 bytes: the index size depends on it");
constexpr size_t MAX_ENTRIES = (INDEX_BYTES - sizeof(Header)) / sizeof(Entry);

const esp_partition_t *s_part = nullptr;
const void            *s_mapped = nullptr;
esp_partition_mmap_handle_t s_map = 0;
Header s_hdr = {};
Entry  s_index[MAX_ENTRIES];
bool   s_ready = false;

bool        s_installing = false;
uint32_t    s_insCount = 0;
uint32_t    s_insManifest = 0;

uint32_t fnv(const uint8_t *p, size_t n, uint32_t h = 2166136261u) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}
uint32_t slug_hash(const char *s) { return fnv((const uint8_t *)s, s ? strlen(s) : 0); }
uint32_t padded(uint32_t len) { return (len + SECTOR - 1) & ~(SECTOR - 1); }

bool map_partition() {
    if (s_mapped) { esp_partition_munmap(s_map); s_mapped = nullptr; s_map = 0; }
    if (esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA, &s_mapped, &s_map) != ESP_OK) {
        printf("[theme_art] mmap failed; the SD/PNG path only\n");
        s_mapped = nullptr;
        return false;
    }
    return true;
}

bool load_index() {
    if (!s_mapped) return false;
    memcpy(&s_hdr, s_mapped, sizeof(Header));
    if (s_hdr.magic != MAGIC || s_hdr.version != VERSION || s_hdr.count > MAX_ENTRIES) {
        if (s_hdr.magic == MAGIC)
            printf("[theme_art] the partition holds a version-%u layout; this firmware reads %u, "
                   "so the active theme is baked again\n", (unsigned)s_hdr.version, (unsigned)VERSION);
        s_hdr = Header{};
        return false;
    }
    memcpy(s_index, (const uint8_t *)s_mapped + sizeof(Header), s_hdr.count * sizeof(Entry));
    return true;
}

// Bytes taken by the distinct blobs among the first n entries.
uint32_t used_bytes(uint32_t n) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i) {
        bool first = true;
        for (uint32_t j = 0; j < i && first; ++j) first = s_index[j].offset != s_index[i].offset;
        if (first) used += padded(s_index[i].len);
    }
    return used;
}

// The first gap of `need` bytes (a multiple of SECTOR) among the blobs the first n entries
// use, or 0 when there is none.
uint32_t first_fit(uint32_t n, uint32_t need) {
    uint32_t at = INDEX_BYTES;
    for (;;) {
        bool moved = false;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t b0 = s_index[i].offset, b1 = b0 + padded(s_index[i].len);
            if (at < b1 && b0 < at + need) { at = b1; moved = true; }   // overlaps: skip past it
        }
        if (!moved) break;
    }
    return at + need <= s_part->size ? at : 0;
}

// Does the flash at `offset` hold exactly these bytes? Read through the partition rather than
// the mapping, which can still show what the cache held before a write.
bool flash_equals(uint32_t offset, const uint8_t *data, size_t len) {
    uint8_t chunk[512];
    for (size_t off = 0; off < len; off += sizeof(chunk)) {
        const size_t n = len - off < sizeof(chunk) ? len - off : sizeof(chunk);
        if (esp_partition_read(s_part, offset + off, chunk, n) != ESP_OK) return false;
        if (memcmp(chunk, data + off, n) != 0) return false;
    }
    return true;
}

// Entries first, magic last: a power cut between the two leaves a blank header, which reads
// as "nothing baked" and sends everything to the SD path.
bool write_index(uint32_t count) {
    if (esp_partition_erase_range(s_part, 0, INDEX_BYTES) != ESP_OK) return false;
    if (count && esp_partition_write(s_part, sizeof(Header), s_index, count * sizeof(Entry)) != ESP_OK)
        return false;
    const Header h = { MAGIC, VERSION, count, used_bytes(count) };
    if (esp_partition_write(s_part, 0, &h, sizeof(h)) != ESP_OK) return false;
    return map_partition() && load_index();
}

const Entry *find(const char *slug, const char *assetName) {
    if (!s_mapped || !s_hdr.count || !slug || !slug[0] || !assetName) return nullptr;
    const uint32_t want = slug_hash(slug);
    for (uint32_t i = 0; i < s_hdr.count; ++i)
        if (s_index[i].slugHash == want && strncmp(s_index[i].name, assetName, sizeof(s_index[i].name)) == 0)
            return &s_index[i];
    return nullptr;
}

}  // namespace

bool begin() {
    if (s_ready) return s_hdr.count > 0;
    s_ready = true;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "themeart");
    if (!s_part) { printf("[theme_art] no themeart partition; the SD/PNG path only\n"); return false; }
    if (!map_partition()) return false;
    const bool ok = load_index();
    printf("[theme_art] partition %u KB, %u baked asset(s) in %u KB, %u KB free\n",
           (unsigned)(s_part->size / 1024), (unsigned)s_hdr.count, (unsigned)(s_hdr.used / 1024),
           (unsigned)(space_free() / 1024));
    return ok && s_hdr.count > 0;
}

bool lookup(const char *slug, const char *assetName, const uint8_t *&out, int &w, int &h, Format &fmt) {
    const Entry *e = find(slug, assetName);
    if (!e) return false;
    out = (const uint8_t *)s_mapped + e->offset;   // straight into flash, never freed
    w = e->w; h = e->h; fmt = (Format)e->fmt;
    return true;
}

bool find_blob(const char *slug, const char *assetName, const uint8_t *&data, size_t &len) {
    const Entry *e = find(slug, assetName);
    if (!e) return false;
    data = (const uint8_t *)s_mapped + e->offset;
    len = e->len;
    return true;
}

bool has(const char *slug, const char *assetName) { return find(slug, assetName) != nullptr; }

bool owns(const void *p) {
    if (!s_mapped || !p) return false;
    const uint8_t *base = (const uint8_t *)s_mapped;
    return (const uint8_t *)p >= base && (const uint8_t *)p < base + s_part->size;
}

bool format_of(const void *p, Format &fmt) {
    if (!owns(p)) return false;
    const uint32_t off = (uint32_t)((const uint8_t *)p - (const uint8_t *)s_mapped);
    for (uint32_t i = 0; i < s_hdr.count; ++i)
        if (s_index[i].offset == off) { fmt = (Format)s_index[i].fmt; return true; }
    return false;
}

const uint8_t *find_active(const char *assetName, Format wantFmt, int &w, int &h) {
    const uint8_t *p = nullptr;
    Format got;
    if (!lookup(theme_select::activeSlug(), assetName, p, w, h, got) || got != wantFmt) return nullptr;
    return p;
}

const uint8_t *find_active_any(const char *assetName, Format wantFmt, int &w, int &h, Format &fmt) {
    const uint8_t *p = nullptr;
    if (!lookup(theme_select::activeSlug(), assetName, p, w, h, fmt)) return nullptr;
    return (fmt == wantFmt || indexed_bpp(fmt)) ? p : nullptr;
}

bool slug_baked(const char *slug) {
    if (!s_mapped || !slug || !slug[0]) return false;
    const uint32_t want = slug_hash(slug);
    for (uint32_t i = 0; i < s_hdr.count; ++i) if (s_index[i].slugHash == want) return true;
    return false;
}

uint32_t baked_manifest(const char *slug) {
    if (!s_mapped || !slug || !slug[0]) return 0;
    const uint32_t want = slug_hash(slug);
    for (uint32_t i = 0; i < s_hdr.count; ++i) if (s_index[i].slugHash == want) return s_index[i].manifest;
    return 0;
}

size_t space_total() { return s_part ? s_part->size - INDEX_BYTES : 0; }
size_t space_free() {
    if (!s_part) return 0;
    return space_total() - used_bytes(s_installing ? s_insCount : s_hdr.count);
}

bool install_begin(const char *slug, uint32_t manifestFingerprint, bool forgetOthers) {
    if (!s_part || !slug || !slug[0]) return false;
    s_insManifest = manifestFingerprint;
    // Keep every other theme's entries (unless told to forget them); this theme's go, and
    // their blobs with them unless another theme shares one.
    uint32_t kept = 0;
    const uint32_t wantSlug = slug_hash(slug);
    for (uint32_t i = 0; i < (forgetOthers ? 0 : s_hdr.count); ++i) {
        if (s_index[i].slugHash == wantSlug) continue;
        if (kept != i) s_index[kept] = s_index[i];
        ++kept;
    }
    if (forgetOthers) printf("[theme_art] forced re-bake: the whole partition is this theme's\n");
    // Until commit writes the magic back, lookup() sees nothing and every caller reads the
    // card: an interrupted install is slow and correct, never half-read.
    if (esp_partition_erase_range(s_part, 0, INDEX_BYTES) != ESP_OK) return false;
    s_hdr = Header{};
    s_insCount = kept;
    s_installing = true;
    if (kept) printf("[theme_art] keeping %u entr(ies) from other themes, %u KB\n",
                     (unsigned)kept, (unsigned)(used_bytes(kept) / 1024));
    return true;
}

bool install_asset(const char *slug, const char *assetName, int w, int h, Format fmt,
                   const uint8_t *data, size_t len) {
    if (!s_installing || !s_part || !data || !len) return false;
    if (s_insCount >= MAX_ENTRIES) {
        printf("[theme_art] index full; %s stays on the SD path\n", assetName);
        return false;
    }
    if (strlen(assetName) >= sizeof(Entry::name)) {
        printf("[theme_art] asset name '%s' is too long for the index (max %u); not baked\n",
               assetName, (unsigned)(sizeof(Entry::name) - 1));
        return false;
    }
    const uint32_t hash = fnv(data, len, fmt * 16777619u + 2166136261u);

    // Already stored, by this theme or another? Then this entry points at it.
    uint32_t offset = 0;
    const char *sharedWith = nullptr;
    for (uint32_t i = 0; i < s_insCount && !offset; ++i) {
        const Entry &e = s_index[i];
        if (e.contentHash == hash && e.len == len && e.fmt == fmt && e.w == w && e.h == h &&
            flash_equals(e.offset, data, len)) {
            offset = e.offset;
            sharedWith = e.name;
        }
    }
    if (!offset) {
        const uint32_t need = padded(len);
        offset = first_fit(s_insCount, need);
        if (!offset) {
            printf("[theme_art] %s/%s (%u KB) does not fit; it stays on the SD path\n",
                   slug, assetName, (unsigned)(len / 1024));
            return false;
        }
        // Each blob owns whole 4 KB sectors, so its erase never reaches into a neighbour (the
        // white-band bug src/theme_art.cpp describes).
        if (esp_partition_erase_range(s_part, offset, need) != ESP_OK) return false;
        if (esp_partition_write(s_part, offset, data, len) != ESP_OK) return false;
        if (!flash_equals(offset, data, len)) {
            printf("[theme_art] %s/%s: verify failed; it stays on the SD path\n", slug, assetName);
            return false;
        }
    }

    Entry &e = s_index[s_insCount++];
    memset(&e, 0, sizeof(e));
    e.slugHash = slug_hash(slug);
    strncpy(e.slugPrefix, slug, sizeof(e.slugPrefix) - 1);
    strncpy(e.name, assetName, sizeof(e.name) - 1);
    e.offset = offset;
    e.len = (uint32_t)len;
    e.w = (uint16_t)w;
    e.h = (uint16_t)h;
    e.fmt = (uint8_t)fmt;
    e.manifest = s_insManifest;
    e.contentHash = hash;
    if (sharedWith) printf("[theme_art] %s: the same pixels as %s, stored once\n", assetName, sharedWith);
    return true;
}

bool install_commit() {
    if (!s_installing || !s_part) return false;
    s_installing = false;
    const bool ok = write_index(s_insCount);
    printf("[theme_art] committed %u asset(s), %u KB used, %u KB free\n",
           (unsigned)s_hdr.count, (unsigned)(s_hdr.used / 1024), (unsigned)(space_free() / 1024));
    return ok;
}

// Drop every entry whose theme is not in `slugs`. Their blobs are free from then on, except
// where another theme shares one. The caller has checked the card is there: a missing card
// lists no themes, which must never read as "every theme was deleted".
int reclaim_missing(const char *const *slugs, int n) {
    if (!s_mapped || !s_hdr.count) return 0;
    uint32_t kept = 0;
    int dropped = 0;
    for (uint32_t i = 0; i < s_hdr.count; ++i) {
        bool installed = false;
        for (int k = 0; k < n && !installed; ++k) installed = s_index[i].slugHash == slug_hash(slugs[k]);
        if (!installed) {
            printf("[theme_art] reclaiming %s/%s: that theme is no longer on the card\n",
                   s_index[i].slugPrefix, s_index[i].name);
            ++dropped;
            continue;
        }
        if (kept != i) s_index[kept] = s_index[i];
        ++kept;
    }
    if (!dropped) return 0;
    const uint32_t before = s_hdr.used;
    if (!write_index(kept)) return -1;
    printf("[theme_art] reclaimed %u KB from themes no longer on the card\n",
           (unsigned)((before - s_hdr.used) / 1024));
    return dropped;
}

}  // namespace theme_art
