/*
 * SolarOS pinyin IME lookup engine over the generated static tables.
 *
 * See solar_os_ime.h.  Table layout (solar_os_ime_table.c):
 *   - solar_os_ime_syllables[]  sorted ascending by pinyin
 *   - solar_os_ime_candidates[] length-prefixed UTF-8 strings
 */

#include <stdbool.h>
#include <string.h>

#include "solar_os_ime.h"
#include "solar_os_ime_table.h"

/* First syllable whose pinyin is >= key (binary search). */
static const solar_os_ime_syllable_t *ime_lower_bound(const char *key)
{
    uint32_t lo = 0;
    uint32_t hi = solar_os_ime_syllable_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (strcmp(solar_os_ime_syllables[mid].pinyin, key) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < solar_os_ime_syllable_count ?
        &solar_os_ime_syllables[lo] : NULL;
}

static int ime_syllable_candidates(const solar_os_ime_syllable_t *syllable,
                                   solar_os_ime_candidate_t *out,
                                   int max_out)
{
    const uint8_t *p = &solar_os_ime_candidates[syllable->cand_off];
    int n = 0;
    int remaining = syllable->cand_count;
    while (remaining-- > 0 && n < max_out) {
        const uint8_t len = *p++;
        out[n].data = p;
        out[n].len = len;
        p += len;
        n++;
    }
    return n;
}

static bool ime_candidate_equal(const solar_os_ime_candidate_t *a,
                                const solar_os_ime_candidate_t *b)
{
    return a->len == b->len &&
        memcmp(a->data, b->data, a->len) == 0;
}

static bool ime_candidate_seen(const solar_os_ime_candidate_t *list,
                               int count,
                               const solar_os_ime_candidate_t *candidate)
{
    for (int i = 0; i < count; i++) {
        if (ime_candidate_equal(&list[i], candidate)) {
            return true;
        }
    }
    return false;
}

int solar_os_ime_exact_count(const char *pinyin)
{
    if (pinyin == NULL || pinyin[0] == '\0') {
        return 0;
    }
    const solar_os_ime_syllable_t *syllable = ime_lower_bound(pinyin);
    if (syllable != NULL && strcmp(syllable->pinyin, pinyin) == 0) {
        return syllable->cand_count;
    }
    return 0;
}

int solar_os_ime_lookup(const char *pinyin,
                        solar_os_ime_candidate_t *out,
                        int max_out)
{
    if (pinyin == NULL || pinyin[0] == '\0' || out == NULL || max_out <= 0) {
        return 0;
    }
    if (max_out > SOLAR_OS_IME_MAX_CANDIDATES) {
        max_out = SOLAR_OS_IME_MAX_CANDIDATES;
    }

    /* Exact syllable first. */
    const solar_os_ime_syllable_t *first = ime_lower_bound(pinyin);
    if (first != NULL && strcmp(first->pinyin, pinyin) == 0) {
        return ime_syllable_candidates(first, out, max_out);
    }

    /* Prefix match across syllables starting at >= pinyin. */
    const size_t pinyin_len = strlen(pinyin);
    const solar_os_ime_syllable_t *end =
        &solar_os_ime_syllables[solar_os_ime_syllable_count];
    int n = 0;
    for (const solar_os_ime_syllable_t *it = first;
         it != NULL && it < end; it++) {
        if (strncmp(it->pinyin, pinyin, pinyin_len) != 0) {
            break;
        }
        solar_os_ime_candidate_t batch[SOLAR_OS_IME_MAX_CANDIDATES];
        const int batch_count =
            ime_syllable_candidates(it, batch, SOLAR_OS_IME_MAX_CANDIDATES);
        for (int i = 0; i < batch_count && n < max_out; i++) {
            if (!ime_candidate_seen(out, n, &batch[i])) {
                out[n++] = batch[i];
            }
        }
    }
    return n;
}