#pragma once

#include <stdint.h>
#include <stdbool.h>

/*
 * Runtime CJK glyph bank.
 *
 * Loads a "FNT1" bitmap font bin from SD card into PSRAM and exposes
 * per-codepoint glyph lookup for the terminal/gfx text renderers.
 *
 * File layout (little-endian):
 *   offset 0  : char[4]  magic = "FNT1"
 *   offset 4  : uint16_t width_hint
 *   offset 6  : uint16_t height_hint
 *   offset 8  : uint32_t glyph_count
 *   offset 12 : directory[glyph_count] of { uint32_t code, uint32_t file_offset }
 *   <data>    : glyph bitmaps, 24 bytes each (12x12, row-major, 2 bytes/row,
 *               12 valid MSB bits left-aligned, low 4 bits zero).
 */

/* Default font bin path on the mounted SD card. */
#define SOLAR_OS_FONTBANK_PATH "/sdcard/fonts/font16.bin"

/* Fallback path on internal flash when SD card is unavailable. */
#define SOLAR_OS_FONTBANK_PATH_FLASH "/flash/fonts/font16.bin"

/* Loaded glyph geometry (FNT1 16px font is 12x12). */
#define SOLAR_OS_FONTBANK_GLYPH_BYTES 24
#define SOLAR_OS_FONTBANK_GLYPH_WIDTH 12
#define SOLAR_OS_FONTBANK_GLYPH_HEIGHT 12

/* Returns true once a font bin has been loaded successfully. */
bool solar_os_fontbank_ready(void);

/*
 * Loads (or reloads) the font bin at the default path. Safe to call from
 * any task after SD is mounted; no-op if already loaded. Returns true on
 * success or when the bank is already loaded.
 */
bool solar_os_fontbank_load(void);

/* Releases the in-memory font buffer. */
void solar_os_fontbank_unload(void);

/*
 * Looks up a glyph for the given Unicode codepoint. Returns a pointer to
 * SOLAR_OS_FONTBANK_GLYPH_BYTES bytes of bitmap data, or NULL if the bank
 * is not loaded or the codepoint has no glyph.
 */
const uint8_t *solar_os_fontbank_glyph(uint32_t codepoint);

/* True if the codepoint is a wide (CJK) character that should use the bank. */
bool solar_os_fontbank_is_wide(uint32_t codepoint);
