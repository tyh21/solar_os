#include "solar_os_fontbank.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

    if (buf[0] != 'F' || buf[1] != 'N' || buf[2] != 'T' || buf[3] != '1') {
        ESP_LOGW(TAG, "bad magic: %02x%02x%02x%02x", buf[0], buf[1], buf[2], buf[3]);
        free(buf);
        return false;
    }

    uint32_t count = read_u32_le(buf + 8);
    if (count == 0 || count > 70000) {
        ESP_LOGW(TAG, "bad glyph count: %u", count);
        free(buf);
        return false;
    }
    uint32_t dir_bytes = count * 8U + 12U;
    if (dir_bytes > (uint32_t)size) {
        ESP_LOGW(TAG, "directory overruns file");
        free(buf);
        return false;
    }

    g_state.data = buf;
    g_state.size = (size_t)size;
    g_state.dir_base = 12;
    g_state.dir_count = count;
    ESP_LOGI(TAG, "loaded %s: %u glyphs, %u bytes", path, count, (unsigned)size);
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
        ok = load_from_path(SOLAR_OS_FONTBANK_PATH);
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
