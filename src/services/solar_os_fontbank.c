#include "solar_os_fontbank.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Font binary embedded into firmware flash via EMBED_FILES. */
extern const uint8_t font16_bin_start[] asm("_binary_font16_bin_start");
extern const uint8_t font16_bin_end[]   asm("_binary_font16_bin_end");

static const char *TAG = "fontbank";

struct solar_os_fontbank_state {
    uint8_t *data;
    size_t size;
    uint32_t dir_base;
    uint32_t dir_count;
    SemaphoreHandle_t lock;
};

static struct solar_os_fontbank_state g_state;

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Validates the FNT1 header and installs the buffer as the active font bank.
 * Takes ownership of *buf on success (caller must NOT free it).
 * On failure *buf is left for the caller to free.
 * Returns true on success.
 */
static bool fontbank_install(uint8_t *buf, size_t size)
{
    if (size < 16) {
        ESP_LOGW(TAG, "font bin too small: %u", (unsigned)size);
        return false;
    }
    if (memcmp(buf, "FNT1", 4) != 0) {
        ESP_LOGW(TAG, "font bin bad magic");
        return false;
    }
    uint32_t glyph_count = read_u32_le(buf + 8);
    if (glyph_count == 0 || glyph_count > 65535) {
        ESP_LOGW(TAG, "font bin bad glyph count: %u", glyph_count);
        return false;
    }
    /* directory starts at offset 12, each entry is 8 bytes */
    size_t dir_end = 12 + (size_t)glyph_count * 8;
    if (dir_end > size) {
        ESP_LOGW(TAG, "font bin directory exceeds file: %u > %u",
                 (unsigned)dir_end, (unsigned)size);
        return false;
    }

    g_state.data = buf;
    g_state.size = size;
    g_state.dir_base = 12;
    g_state.dir_count = glyph_count;
    return true;
}

static bool load_from_path(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "font bin not found: %s", path);
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 12 || size > (2 * 1024 * 1024)) {
        ESP_LOGW(TAG, "font bin bad size: %ld", size);
        fclose(f);
        return false;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)size,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "PSRAM alloc failed (%ld bytes)", size);
        fclose(f);
        return false;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        ESP_LOGW(TAG, "short read: %u/%ld", (unsigned)got, size);
        free(buf);
        return false;
    }

    if (!fontbank_install(buf, (size_t)size)) {
        free(buf);
        return false;
    }
    ESP_LOGI(TAG, "loaded %s: %u glyphs, %u bytes", path, g_state.dir_count, (unsigned)g_state.size);
    return true;
}

static bool load_from_embedded(void)
{
    const size_t size = (size_t)(font16_bin_end - font16_bin_start);
    if (size <= 12 || size > (2 * 1024 * 1024)) {
        ESP_LOGW(TAG, "embedded font bin bad size: %u", (unsigned)size);
        return false;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(size,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "PSRAM alloc failed (%u bytes)", (unsigned)size);
        return false;
    }

    memcpy(buf, font16_bin_start, size);

    if (!fontbank_install(buf, size)) {
        free(buf);
        return false;
    }
    ESP_LOGI(TAG, "loaded embedded: %u glyphs, %u bytes", g_state.dir_count, (unsigned)g_state.size);
    return true;
}

bool solar_os_fontbank_load(void)
{
    if (g_state.lock == NULL) {
        g_state.lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(g_state.lock, portMAX_DELAY);
    bool ok = (g_state.data != NULL);
    if (!ok) {
        /* 1. Embedded font bin in firmware flash (always available) */
        ok = load_from_embedded();
        /* 2. SD card (user can override with a different font) */
        if (!ok) {
            ok = load_from_path(SOLAR_OS_FONTBANK_PATH);
        }
        /* 3. Internal flash FATFS partition */
        if (!ok) {
            ok = load_from_path(SOLAR_OS_FONTBANK_PATH_FLASH);
        }
    }
    xSemaphoreGive(g_state.lock);
    return ok;
}

bool solar_os_fontbank_ready(void)
{
    return g_state.data != NULL;
}

void solar_os_fontbank_unload(void)
{
    if (g_state.lock != NULL) {
        xSemaphoreTake(g_state.lock, portMAX_DELAY);
    }
    if (g_state.data != NULL) {
        free(g_state.data);
        g_state.data = NULL;
        g_state.size = 0;
        g_state.dir_count = 0;
    }
    if (g_state.lock != NULL) {
        xSemaphoreGive(g_state.lock);
    }
}

const uint8_t *solar_os_fontbank_glyph(uint32_t codepoint)
{
    if (g_state.data == NULL) {
        if (!solar_os_fontbank_load()) {
            return NULL;
        }
    }
    if (g_state.data == NULL || g_state.dir_count == 0) {
        return NULL;
    }

    const uint8_t *base = g_state.data;
    uint32_t lo = 0;
    uint32_t hi = g_state.dir_count - 1;
    while (lo <= hi) {
        uint32_t mid = lo + (hi - lo) / 2U;
        const uint8_t *entry = base + g_state.dir_base + (size_t)mid * 8U;
        uint32_t code = read_u32_le(entry);
        if (code == codepoint) {
            uint32_t off = read_u32_le(entry + 4);
            if (off + SOLAR_OS_FONTBANK_GLYPH_BYTES > g_state.size) {
                return NULL;
            }
            return base + off;
        }
        if (code < codepoint) {
            lo = mid + 1U;
        } else {
            if (mid == 0) {
                break;
            }
            hi = mid - 1U;
        }
    }
    return NULL;
}

bool solar_os_fontbank_is_wide(uint32_t codepoint)
{
    /* East Asian wide ranges that the FNT1 bank actually covers. */
    return (codepoint >= 0x1100 && codepoint <= 0x115F) ||
           (codepoint >= 0x2E80 && codepoint <= 0x303E) ||
           (codepoint >= 0x3041 && codepoint <= 0x33FF) ||
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
           (codepoint >= 0xA000 && codepoint <= 0xA4CF) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
           (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
           (codepoint >= 0xFE30 && codepoint <= 0xFE4F) ||
           (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
           (codepoint >= 0xFFE0 && codepoint <= 0xFFE6);
}
