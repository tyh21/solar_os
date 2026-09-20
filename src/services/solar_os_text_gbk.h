#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Text encoding helpers for CJK rendering.
 *
 * FatFS is configured with codepage 936 (GBK) and ANSI_OEM API encoding,
 * so file names read from SD arrive as GBK byte strings. The UI renderers
 * work in Unicode, so this module converts GBK byte streams to UTF-8 and
 * provides UTF-8/codepoint utilities shared by gfx and terminal.
 */

/* Maximum UTF-8 bytes for one Unicode codepoint. */
#define SOLAR_OS_TEXT_UTF8_MAX_BYTES 4

/*
 * Converts a GBK byte string to a NUL-terminated UTF-8 string. Returns the
 * number of bytes written to dst (excluding the NUL). dst_size must include
 * room for the NUL. src_len of SIZE_MAX treats src as NUL-terminated.
 */
size_t solar_os_text_gbk_to_utf8(const char *src, size_t src_len,
                                 char *dst, size_t dst_size);

/* Decodes one UTF-8 codepoint from *p (up to end). Advances *p and returns the
 * codepoint, or 0xFFFD on invalid input. */
uint32_t solar_os_text_utf8_next(const char **p, const char *end);

/* Encodes a codepoint as UTF-8 into out (>= 4 bytes). Returns byte count. */
int solar_os_text_utf8_from_codepoint(uint32_t cp, char *out);

/* True for East Asian wide codepoints that should render double-width. */
bool solar_os_text_is_wide(uint32_t codepoint);
