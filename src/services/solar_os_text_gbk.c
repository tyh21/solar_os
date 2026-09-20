#include "solar_os_text_gbk.h"

#include <string.h>

/* Generated GBK -> Unicode table (see fonts/gen_gbk_table.py). */
#include "solar_os_gbk_table.h"

#define GBK_HI_MIN 0x81
#define GBK_HI_MAX 0xFE
#define GBK_LO_COLS 190  /* 0x40..0x7E (63) + 0x80..0xFE (127) */

static size_t gbk_index(uint8_t hi, uint8_t lo)
{
    if (hi < GBK_HI_MIN || hi > GBK_HI_MAX) {
        return (size_t)-1;
    }
    if (lo < 0x40 || lo > 0xFE || lo == 0x7F) {
        return (size_t)-1;
    }
    size_t col = (size_t)(lo - 0x40);
    if (lo > 0x7F) {
        col -= 1;  /* skip the 0x7F hole */
    }
    return (size_t)(hi - GBK_HI_MIN) * GBK_LO_COLS + col;
}

static int utf8_emit(uint32_t cp, char *out)
{
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
}

size_t solar_os_text_gbk_to_utf8(const char *src, size_t src_len,
                                 char *dst, size_t dst_size)
{
    if (dst == NULL || dst_size == 0) {
        return 0;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }
    if (src_len == SIZE_MAX) {
        src_len = strlen(src);
    }

    size_t out = 0;
    size_t i = 0;
    while (i < src_len) {
        uint8_t b = (uint8_t)src[i];
        int written;
        if (b < 0x80) {
            if (out + 1 + 1 > dst_size) {
                break;
            }
            dst[out++] = (char)b;
            i++;
            continue;
        }

        uint32_t cp = 0xFFFD;
        if (i + 1 < src_len) {
            uint8_t b2 = (uint8_t)src[i + 1];
            size_t idx = gbk_index(b, b2);
            if (idx != (size_t)-1) {
                uint16_t u = solar_os_gbk_to_unicode[idx];
                if (u != 0) {
                    cp = u;
                }
            }
        }

        char tmp[SOLAR_OS_TEXT_UTF8_MAX_BYTES];
        written = utf8_emit(cp, tmp);
        if (out + (size_t)written + 1 > dst_size) {
            break;
        }
        memcpy(dst + out, tmp, (size_t)written);
        out += (size_t)written;
        i += (b >= 0x81 && b <= 0xFE && i + 1 < src_len) ? 2 : 1;
    }

    dst[out] = '\0';
    return out;
}

uint32_t solar_os_text_utf8_next(const char **p, const char *end)
{
    if (p == NULL || *p == NULL || *p >= end) {
        return 0xFFFD;
    }
    const uint8_t *s = (const uint8_t *)*p;
    uint8_t b = s[0];
    uint32_t cp;
    int len;

    if (b < 0x80) {
        cp = b;
        len = 1;
    } else if ((b & 0xE0) == 0xC0) {
        cp = b & 0x1F;
        len = 2;
    } else if ((b & 0xF0) == 0xE0) {
        cp = b & 0x0F;
        len = 3;
    } else if ((b & 0xF8) == 0xF0) {
        cp = b & 0x07;
        len = 4;
    } else {
        *p = *p + 1;
        return 0xFFFD;
    }

    if (s + len > (const uint8_t *)end) {
        *p = end;
        return 0xFFFD;
    }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p = (const char *)(s + i);
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p = (const char *)(s + len);
    return cp;
}

int solar_os_text_utf8_from_codepoint(uint32_t cp, char *out)
{
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

bool solar_os_text_is_wide(uint32_t codepoint)
{
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
