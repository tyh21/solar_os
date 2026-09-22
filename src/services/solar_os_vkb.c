/*
 * On-screen virtual keyboard for touch-equipped SolarOS boards.
 *
 * See solar_os_vkb.h for the public interface.  This file is compiled
 * only when the board declares SOLAR_OS_BOARD_HAS_POINTER (handled in
 * CMakeLists.txt).
 */

#include "solar_os_vkb.h"

#if SOLAR_OS_BOARD_HAS_POINTER

#include <string.h>
#include "u8g2.h"
#include "solar_os_fonts.h"
#include "solar_os_fontbank.h"
#include "solar_os_text_gbk.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define VKB_MARGIN_X         2       /* side margin                           */
#define VKB_TRIGGER_HEIGHT   16      /* trigger strip height (hidden)        */
#define VKB_KB_FRAC_NUM      2       /* keyboard height = display * num/den   */
#define VKB_KB_FRAC_DEN      5
#define VKB_ROW_COUNT        4
#define VKB_FONT             u8g2_font_solar_os_default_r_12_tf
#define VKB_KEY_GAP          1       /* pixel gap between keys                */
#define VKB_ROW_GAP          1       /* pixel gap between rows                */

/* ------------------------------------------------------------------ */
/* Key definition tables                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *label;       /* NULL terminates the row                */
    uint8_t weight;          /* relative width                         */
    uint8_t action_type;     /* solar_os_vkb_action_type_t            */
    char ch;                 /* payload for ACTION_CHAR               */
} vkb_kdef_t;

/* ---- Pinyin IME mode ---- */

void solar_os_vkb_set_mode(solar_os_vkb_t *vkb, solar_os_vkb_mode_t mode)
{
    if (vkb == NULL) {
        return;
    }
    if (mode != vkb->mode) {
        vkb->mode = mode;
        vkb->layout_valid = false;
    }
}

void solar_os_vkb_set_candidates(solar_os_vkb_t *vkb,
                                 const char *const *candidates,
                                 int count)
{
    if (vkb == NULL) {
        return;
    }
    if (count > SOLAR_OS_IME_PAGE_SIZE) {
        count = SOLAR_OS_IME_PAGE_SIZE;
    }
    int n = 0;
    for (; n < count; n++) {
        if (candidates == NULL || candidates[n] == NULL) {
            break;
        }
        vkb->ime_candidates[n] = candidates[n];
    }
    for (int i = n; i < SOLAR_OS_IME_PAGE_SIZE; i++) {
        vkb->ime_candidates[i] = NULL;
    }
    vkb->ime_candidate_count = n;
    vkb->layout_valid = false;
}

int solar_os_vkb_candidate_count(const solar_os_vkb_t *vkb)
{
    if (vkb == NULL || vkb->mode != SOLAR_OS_VKB_MODE_PINYIN) {
        return 0;
    }
    return vkb->ime_candidate_count;
}

/* UTF-8/CJK aware text width at the vkb font metrics. */
static int vkb_text_width(u8g2_t *u8g2, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    const int cell = u8g2_GetMaxCharWidth(u8g2);
    int width = 0;
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end) {
        uint32_t cp = solar_os_text_utf8_next(&p, end);
        if (solar_os_fontbank_is_wide(cp) && solar_os_fontbank_glyph(cp) != NULL) {
            int adv = cell * 2;
            if (adv < SOLAR_OS_FONTBANK_GLYPH_WIDTH) {
                adv = SOLAR_OS_FONTBANK_GLYPH_WIDTH;
            }
            width += adv;
        } else {
            width += u8g2_GetGlyphWidth(u8g2, (uint16_t)(cp & 0xFFFFU));
        }
    }
    return width;
}

/* Draw mixed ASCII/CJK text at baseline y. Returns end x. */
static int vkb_draw_text(u8g2_t *u8g2, int x, int baseline_y, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return x;
    }
    const int cell = u8g2_GetMaxCharWidth(u8g2);
    const int ascent = (int)u8g2_GetAscent(u8g2);
    int cx = x;
    const char *p = text;
    const char *end = text + strlen(text);
    while (p < end) {
        uint32_t cp = solar_os_text_utf8_next(&p, end);
        const uint8_t *g = solar_os_fontbank_is_wide(cp)
                               ? solar_os_fontbank_glyph(cp) : NULL;
        if (g != NULL) {
            int adv = cell * 2;
            if (adv < SOLAR_OS_FONTBANK_GLYPH_WIDTH) {
                adv = SOLAR_OS_FONTBANK_GLYPH_WIDTH;
            }
            int top = baseline_y - ascent +
                      (ascent > SOLAR_OS_FONTBANK_GLYPH_HEIGHT
                           ? (ascent - SOLAR_OS_FONTBANK_GLYPH_HEIGHT) / 2
                           : 0);
            u8g2_DrawBitmap(u8g2, (u8g2_uint_t)cx, (u8g2_uint_t)top,
                            (u8g2_uint_t)(SOLAR_OS_FONTBANK_GLYPH_BYTES /
                                          SOLAR_OS_FONTBANK_GLYPH_HEIGHT),
                            (u8g2_uint_t)SOLAR_OS_FONTBANK_GLYPH_HEIGHT,
                            g);
            cx += adv;
        } else {
            u8g2_DrawGlyph(u8g2, (u8g2_uint_t)cx, (u8g2_uint_t)baseline_y,
                           (uint16_t)(cp & 0xFFFFU));
            cx += u8g2_GetGlyphWidth(u8g2, (uint16_t)(cp & 0xFFFFU));
        }
    }
    return cx;
}

/* helper: shortcut for letter keys: KC('q') -> { "q", 1, ..., 'q' } */
#define KC(c)   { (const char[]){(c),'\0'}, 1, SOLAR_OS_VKB_ACTION_CHAR, (char)(c) }
/* helper: shortcut for special keys */
#define KS(l,w,t) { (l), (w), (t), 0 }
/* helper: shortcut for symbol keys (same as KC, kept for readability) */
#define KSY(c) KC(c)

/* ---- Lowercase mode ---- */
static const vkb_kdef_t row_lower_0[] = {
    KS("1#",2,SOLAR_OS_VKB_ACTION_SYMBOL),
    KC('q'), KC('w'), KC('e'), KC('r'), KC('t'), KC('y'), KC('u'), KC('i'), KC('o'), KC('p'),
    KS("BS",2,SOLAR_OS_VKB_ACTION_BACKSPACE),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_lower_1[] = {
    KS("ABC",2,SOLAR_OS_VKB_ACTION_SHIFT),
    KC('a'), KC('s'), KC('d'), KC('f'), KC('g'), KC('h'), KC('j'), KC('k'), KC('l'),
    KS("CR",2,SOLAR_OS_VKB_ACTION_ENTER),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_lower_2[] = {
    KC('_'), KC('-'), KC('z'), KC('x'), KC('c'), KC('v'), KC('b'), KC('n'), KC('m'),
    KC('.'), KC(','), KC(':'),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_lower_3[] = {
    KS("KB",2,SOLAR_OS_VKB_ACTION_TOGGLE),
    KS("<",2,SOLAR_OS_VKB_ACTION_LEFT),
    KS("space",4,SOLAR_OS_VKB_ACTION_SPACE),
    KS(">",2,SOLAR_OS_VKB_ACTION_RIGHT),
    KS("OK",2,SOLAR_OS_VKB_ACTION_ENTER),
    {NULL,0,0,0},
};

/* ---- Uppercase mode ---- */
static const vkb_kdef_t row_upper_0[] = {
    KS("1#",2,SOLAR_OS_VKB_ACTION_SYMBOL),
    KC('Q'), KC('W'), KC('E'), KC('R'), KC('T'), KC('Y'), KC('U'), KC('I'), KC('O'), KC('P'),
    KS("BS",2,SOLAR_OS_VKB_ACTION_BACKSPACE),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_upper_1[] = {
    KS("abc",2,SOLAR_OS_VKB_ACTION_SHIFT),
    KC('A'), KC('S'), KC('D'), KC('F'), KC('G'), KC('H'), KC('J'), KC('K'), KC('L'),
    KS("CR",2,SOLAR_OS_VKB_ACTION_ENTER),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_upper_2[] = {
    KC('_'), KC('-'), KC('Z'), KC('X'), KC('C'), KC('V'), KC('B'), KC('N'), KC('M'),
    KC('.'), KC(','), KC(':'),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_upper_3[] = {
    KS("KB",2,SOLAR_OS_VKB_ACTION_TOGGLE),
    KS("<",2,SOLAR_OS_VKB_ACTION_LEFT),
    KS("space",4,SOLAR_OS_VKB_ACTION_SPACE),
    KS(">",2,SOLAR_OS_VKB_ACTION_RIGHT),
    KS("OK",2,SOLAR_OS_VKB_ACTION_ENTER),
    {NULL,0,0,0},
};

/* ---- Pinyin IME mode ---- */
/* Row 0 is the candidate row: page-prev, 8 candidates, page-next.  The
 * candidate labels/actions are rebuilt at runtime from vkb->candidate_*;
 * the entries here are placeholders with weight 1 each. */
static const vkb_kdef_t row_py_0[] = {
    KS("<",1,SOLAR_OS_VKB_ACTION_IME_PAGE_PREV),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(" ",1,SOLAR_OS_VKB_ACTION_IME_SELECT),
    KS(">",1,SOLAR_OS_VKB_ACTION_IME_PAGE_NEXT),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_py_1[] = {
    KC('q'), KC('w'), KC('e'), KC('r'), KC('t'), KC('y'), KC('u'), KC('i'), KC('o'), KC('p'),
    KS("BS",2,SOLAR_OS_VKB_ACTION_BACKSPACE),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_py_2[] = {
    KC('a'), KC('s'), KC('d'), KC('f'), KC('g'), KC('h'), KC('j'), KC('k'), KC('l'),
    KS("CR",2,SOLAR_OS_VKB_ACTION_ENTER),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_py_3[] = {
    KS("abc",2,SOLAR_OS_VKB_ACTION_IME_ABC),
    KC('z'), KC('x'), KC('c'), KC('v'), KC('b'), KC('n'), KC('m'),
    KS("space",3,SOLAR_OS_VKB_ACTION_SPACE),
    KS("KB",2,SOLAR_OS_VKB_ACTION_TOGGLE),
    {NULL,0,0,0},
};

/* ---- Symbol mode ---- */
static const vkb_kdef_t row_sym_0[] = {
    KS("abc",2,SOLAR_OS_VKB_ACTION_SYMBOL),
    KC('1'), KC('2'), KC('3'), KC('4'), KC('5'), KC('6'), KC('7'), KC('8'), KC('9'), KC('0'),
    KS("BS",2,SOLAR_OS_VKB_ACTION_BACKSPACE),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_sym_1[] = {
    KS("AB",2,SOLAR_OS_VKB_ACTION_SHIFT),
    KSY('!'), KSY('@'), KSY('#'), KSY('$'), KSY('%'), KSY('^'), KSY('&'), KSY('*'),
    KS("CR",2,SOLAR_OS_VKB_ACTION_ENTER),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_sym_2[] = {
    KSY('('), KSY(')'), KSY('-'), KSY('='), KSY('+'), KSY('/'),
    KSY(';'), KSY('\''), KSY('"'), KSY('?'), KSY('<'), KSY('>'),
    {NULL,0,0,0},
};
static const vkb_kdef_t row_sym_3[] = {
    KS("KB",2,SOLAR_OS_VKB_ACTION_TOGGLE),
    KS("<",2,SOLAR_OS_VKB_ACTION_LEFT),
    KS("space",4,SOLAR_OS_VKB_ACTION_SPACE),
    KS(">",2,SOLAR_OS_VKB_ACTION_RIGHT),
    KS("OK",2,SOLAR_OS_VKB_ACTION_ENTER),
    {NULL,0,0,0},
};

static const vkb_kdef_t *const mode_rows[4][VKB_ROW_COUNT] = {
    { row_lower_0, row_lower_1, row_lower_2, row_lower_3 },
    { row_upper_0, row_upper_1, row_upper_2, row_upper_3 },
    { row_sym_0,   row_sym_1,   row_sym_2,   row_sym_3   },
    { row_py_0,    row_py_1,    row_py_2,    row_py_3    },
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint8_t vkb_polarity(bool palette_inverted,
                            bool black_is_one,
                            uint8_t white_bit)
{
    return (black_is_one != palette_inverted) ? (uint8_t)!white_bit : white_bit;
}

static void vkb_set_color(u8g2_t *u8g2,
                          bool palette_inverted,
                          bool black_is_one,
                          uint8_t white_bit)
{
    u8g2_SetDrawColor(u8g2, vkb_polarity(palette_inverted, black_is_one, white_bit));
}

static int vkb_kb_height(int display_height)
{
    int h = display_height * VKB_KB_FRAC_NUM / VKB_KB_FRAC_DEN;
    if (h < 4 * 20) h = 4 * 20;   /* minimum 20px per row */
    return h;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void vkb_rebuild_keys(solar_os_vkb_t *vkb)
{
    const vkb_kdef_t *const *rows = mode_rows[vkb->mode];
    for (int r = 0; r < VKB_ROW_COUNT; r++) {
        solar_os_vkb_row_t *row = &vkb->rows[r];
        const vkb_kdef_t *def = rows[r];
        int n = 0;
        int select_index = 0;
        while (def->label != NULL && n < SOLAR_OS_VKB_MAX_ROW_KEYS) {
            solar_os_vkb_key_t *key = &row->keys[n];
            key->label  = def->label;
            key->action.type = (solar_os_vkb_action_type_t)def->action_type;
            key->action.ch    = def->ch;
            key->highlighted  = false;

            if (vkb->mode == SOLAR_OS_VKB_MODE_PINYIN && r == 0) {
                /* candidate keys: label + select index from the current
                 * candidate list (skipping the page-flip keys). */
                if (key->action.type == SOLAR_OS_VKB_ACTION_IME_SELECT) {
                    const int index = select_index++;
                    key->action.ch = (char)index;
                    key->label = (index < vkb->ime_candidate_count)
                                     ? vkb->ime_candidates[index] : "";
                }
            }
            n++;
            def++;
        }
        row->count = n;
    }
}

void solar_os_vkb_layout(solar_os_vkb_t *vkb, int display_width, int display_height)
{
    if (vkb == NULL || display_width <= 0 || display_height <= 0) {
        return;
    }

    vkb->display_width  = display_width;
    vkb->display_height = display_height;
    vkb->keyboard_height = vkb_kb_height(display_height);

    /* trigger rect (used when keyboard is hidden) */
    vkb->trigger_w = 36;
    vkb->trigger_h = VKB_TRIGGER_HEIGHT;
    vkb->trigger_x = display_width - vkb->trigger_w - VKB_MARGIN_X;
    vkb->trigger_y = display_height - vkb->trigger_h;

    /* rebuild key definitions if mode changed */
    vkb_rebuild_keys(vkb);

    /* compute pixel positions for each key */
    const int usable_w = display_width - 2 * VKB_MARGIN_X;
    const int row_h = vkb->keyboard_height / VKB_ROW_COUNT;

    for (int r = 0; r < VKB_ROW_COUNT; r++) {
        solar_os_vkb_row_t *row = &vkb->rows[r];
        row->y_top = display_height - vkb->keyboard_height + r * row_h;

        /* read weights from the static table */
        const vkb_kdef_t *def = mode_rows[vkb->mode][r];
        int total_weight = 0;
        for (int i = 0; i < row->count; i++) {
            total_weight += def[i].weight;
        }

        int x = VKB_MARGIN_X;
        int remaining = usable_w;
        for (int i = 0; i < row->count; i++) {
            int w;
            if (i == row->count - 1) {
                w = remaining;   /* last key absorbs rounding error */
            } else {
                w = usable_w * def[i].weight / total_weight;
                remaining -= w;
            }
            row->keys[i].x = x;
            row->keys[i].y = row->y_top;
            row->keys[i].w = w;
            row->keys[i].h = row_h;
            x += w;
        }
    }

    vkb->layout_valid = true;
}

/* ------------------------------------------------------------------ */
/* Public: init / visibility                                           */
/* ------------------------------------------------------------------ */

void solar_os_vkb_init(solar_os_vkb_t *vkb)
{
    if (vkb == NULL) return;
    memset(vkb, 0, sizeof(*vkb));
    vkb->visible = false;
    vkb->pointer_down = false;
    vkb->pressed_key_row = -1;
    vkb->pressed_key_col = -1;
    vkb->mode = SOLAR_OS_VKB_MODE_LOWER;
    vkb->prev_mode = SOLAR_OS_VKB_MODE_LOWER;
    vkb->layout_valid = false;
    vkb->keyboard_height = 0;
    vkb->display_width = 0;
    vkb->display_height = 0;
}

bool solar_os_vkb_is_visible(const solar_os_vkb_t *vkb)
{
    return vkb != NULL && vkb->visible;
}

bool solar_os_vkb_set_visible(solar_os_vkb_t *vkb, bool visible)
{
    if (vkb == NULL) return false;
    bool changed = vkb->visible != visible;
    vkb->visible = visible;
    if (changed) {
        vkb->pressed_key_row = -1;
        vkb->pressed_key_col = -1;
        vkb->pointer_down = false;
    }
    return changed;
}

bool solar_os_vkb_toggle(solar_os_vkb_t *vkb)
{
    if (vkb == NULL) return false;
    solar_os_vkb_set_visible(vkb, !vkb->visible);
    return vkb->visible;
}

int solar_os_vkb_overlay_height(const solar_os_vkb_t *vkb, int display_height)
{
    if (vkb == NULL) return 0;
    if (vkb->visible) {
        return vkb_kb_height(display_height);
    }
    return VKB_TRIGGER_HEIGHT;
}

void solar_os_vkb_trigger_rect(const solar_os_vkb_t *vkb,
                               int *x, int *y, int *w, int *h)
{
    if (vkb == NULL) return;
    if (x) *x = vkb->trigger_x;
    if (y) *y = vkb->trigger_y;
    if (w) *w = vkb->trigger_w;
    if (h) *h = vkb->trigger_h;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

void solar_os_vkb_draw(solar_os_vkb_t *vkb,
                       void *u8g2_ptr,
                       bool palette_inverted,
                       bool black_is_one)
{
    if (vkb == NULL || u8g2_ptr == NULL) return;
    u8g2_t *u8g2 = (u8g2_t *)u8g2_ptr;

    if (!vkb->layout_valid) {
        solar_os_vkb_layout(vkb,
                            u8g2_GetDisplayWidth(u8g2),
                            u8g2_GetDisplayHeight(u8g2));
    }
    if (!vkb->layout_valid) return;

    const int dw = vkb->display_width;
    const int dh = vkb->display_height;

    u8g2_SetFont(u8g2, VKB_FONT);
    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetFontPosBaseline(u8g2);
    const int ascent = u8g2_GetAscent(u8g2);

    if (!vkb->visible) {
        /* ---- trigger strip only ---- */
        const int top = dh - VKB_TRIGGER_HEIGHT;
        /* fill strip background (dark) */
        vkb_set_color(u8g2, palette_inverted, black_is_one, 0);
        u8g2_DrawBox(u8g2, 0, (u8g2_uint_t)top, (u8g2_uint_t)dw, VKB_TRIGGER_HEIGHT);

        /* draw trigger button (light box with dark text) */
        const int tx = vkb->trigger_x;
        const int ty = vkb->trigger_y;
        const int tw = vkb->trigger_w;
        const int th = vkb->trigger_h;
        vkb_set_color(u8g2, palette_inverted, black_is_one, 1);
        u8g2_DrawBox(u8g2, (u8g2_uint_t)tx, (u8g2_uint_t)ty,
                     (u8g2_uint_t)tw, (u8g2_uint_t)th);
        vkb_set_color(u8g2, palette_inverted, black_is_one, 0);
        const char *lbl = "KB";
        int lw = u8g2_GetStrWidth(u8g2, lbl);
        int lx = tx + (tw - lw) / 2;
        int ly = ty + (th + ascent) / 2;
        u8g2_DrawStr(u8g2, (u8g2_uint_t)lx, (u8g2_uint_t)ly, lbl);

        /* draw a thin line above the trigger strip */
        vkb_set_color(u8g2, palette_inverted, black_is_one, 1);
        u8g2_DrawHLine(u8g2, 0, (u8g2_uint_t)(top - 1), (u8g2_uint_t)dw);
        vkb_set_color(u8g2, palette_inverted, black_is_one, 0);
        return;
    }

    /* ---- full keyboard ---- */
    const int kb_top = dh - vkb->keyboard_height;

    /* fill keyboard background (dark) */
    vkb_set_color(u8g2, palette_inverted, black_is_one, 0);
    u8g2_DrawBox(u8g2, 0, (u8g2_uint_t)kb_top,
                 (u8g2_uint_t)dw, (u8g2_uint_t)vkb->keyboard_height);

    /* thin separator line above keyboard */
    vkb_set_color(u8g2, palette_inverted, black_is_one, 1);
    u8g2_DrawHLine(u8g2, 0, (u8g2_uint_t)(kb_top - 1), (u8g2_uint_t)dw);

    /* grid lines between rows (light) */
    const int row_h = vkb->keyboard_height / VKB_ROW_COUNT;
    for (int r = 1; r < VKB_ROW_COUNT; r++) {
        int ly = kb_top + r * row_h;
        vkb_set_color(u8g2, palette_inverted, black_is_one, 1);
        u8g2_DrawHLine(u8g2, 0, (u8g2_uint_t)ly, (u8g2_uint_t)dw);
    }

    /* draw keys */
    for (int r = 0; r < VKB_ROW_COUNT; r++) {
        solar_os_vkb_row_t *row = &vkb->rows[r];
        for (int i = 0; i < row->count; i++) {
            solar_os_vkb_key_t *key = &row->keys[i];
            const char *lbl = key->label;
            if (lbl == NULL) continue;

            /* vertical separator to the left of this key (light) */
            if (i > 0) {
                vkb_set_color(u8g2, palette_inverted, black_is_one, 1);
                u8g2_DrawVLine(u8g2, (u8g2_uint_t)key->x,
                               (u8g2_uint_t)row->y_top,
                               (u8g2_uint_t)row_h);
            }

            if (key->highlighted) {
                /* pressed: draw light box, then dark text */
                vkb_set_color(u8g2, palette_inverted, black_is_one, 1);
                u8g2_DrawBox(u8g2,
                             (u8g2_uint_t)(key->x + 1),
                             (u8g2_uint_t)(key->y + 1),
                             (u8g2_uint_t)(key->w - 2),
                             (u8g2_uint_t)(key->h - 2));
                vkb_set_color(u8g2, palette_inverted, black_is_one, 0);
            } else {
                vkb_set_color(u8g2, palette_inverted, black_is_one, 1);
            }

            int lw = vkb_text_width(u8g2, lbl);
            int lx = key->x + (key->w - lw) / 2;
            int ly = key->y + (key->h + ascent) / 2;
            if (lx < key->x + 1) lx = key->x + 1;
            (void)vkb_draw_text(u8g2, lx, ly, lbl);
        }
    }

    vkb_set_color(u8g2, palette_inverted, black_is_one, 0);
}

/* ------------------------------------------------------------------ */
/* Hit testing                                                          */
/* ------------------------------------------------------------------ */

static bool vkb_key_at(solar_os_vkb_t *vkb, int x, int y,
                       int *out_row, int *out_col)
{
    if (!vkb->layout_valid || !vkb->visible) return false;
    const int kb_top = vkb->display_height - vkb->keyboard_height;
    if (y < kb_top || y >= vkb->display_height) return false;
    const int row_h = vkb->keyboard_height / VKB_ROW_COUNT;
    int r = (y - kb_top) / row_h;
    if (r < 0 || r >= VKB_ROW_COUNT) return false;
    solar_os_vkb_row_t *row = &vkb->rows[r];
    for (int i = 0; i < row->count; i++) {
        solar_os_vkb_key_t *key = &row->keys[i];
        if (x >= key->x && x < key->x + key->w) {
            *out_row = r;
            *out_col = i;
            return true;
        }
    }
    return false;
}

static bool vkb_trigger_hit(const solar_os_vkb_t *vkb, int x, int y)
{
    return x >= vkb->trigger_x && x < vkb->trigger_x + vkb->trigger_w &&
           y >= vkb->trigger_y && y < vkb->trigger_y + vkb->trigger_h;
}

static void vkb_clear_highlight(solar_os_vkb_t *vkb)
{
    if (vkb->pressed_key_row >= 0 && vkb->pressed_key_row < VKB_ROW_COUNT) {
        solar_os_vkb_row_t *row = &vkb->rows[vkb->pressed_key_row];
        if (vkb->pressed_key_col >= 0 && vkb->pressed_key_col < row->count) {
            row->keys[vkb->pressed_key_col].highlighted = false;
        }
    }
    vkb->pressed_key_row = -1;
    vkb->pressed_key_col = -1;
}

static void vkb_apply_mode_action(solar_os_vkb_t *vkb, solar_os_vkb_action_type_t type)
{
    switch (type) {
    case SOLAR_OS_VKB_ACTION_SHIFT:
        if (vkb->mode == SOLAR_OS_VKB_MODE_UPPER) {
            vkb->mode = SOLAR_OS_VKB_MODE_LOWER;
        } else {
            vkb->prev_mode = vkb->mode;
            vkb->mode = SOLAR_OS_VKB_MODE_UPPER;
        }
        vkb->layout_valid = false;   /* rebuild keys for new mode */
        break;
    case SOLAR_OS_VKB_ACTION_SYMBOL:
        if (vkb->mode == SOLAR_OS_VKB_MODE_SYMBOL) {
            vkb->mode = vkb->prev_mode;
        } else {
            vkb->prev_mode = vkb->mode;
            vkb->mode = SOLAR_OS_VKB_MODE_SYMBOL;
        }
        vkb->layout_valid = false;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Pointer handling                                                    */
/* ------------------------------------------------------------------ */

bool solar_os_vkb_handle_pointer(solar_os_vkb_t *vkb,
                                 solar_os_input_pointer_action_t action,
                                 int x, int y,
                                 solar_os_vkb_action_t *out_action)
{
    if (vkb == NULL || out_action == NULL) return false;

    out_action->type = SOLAR_OS_VKB_ACTION_NONE;
    out_action->ch = 0;

    if (!vkb->layout_valid) {
        solar_os_vkb_layout(vkb, vkb->display_width, vkb->display_height);
        if (!vkb->layout_valid) {
            /* still no dimensions — accept press on trigger area only */
            if (action == SOLAR_OS_INPUT_POINTER_PRESS &&
                x >= 0 && y >= 0) {
                /* can't do much without layout, but let caller handle */
            }
            return false;
        }
    }

    /* ---- keyboard hidden: only trigger strip is active ---- */
    if (!vkb->visible) {
        if (action == SOLAR_OS_INPUT_POINTER_PRESS && vkb_trigger_hit(vkb, x, y)) {
            solar_os_vkb_set_visible(vkb, true);
            out_action->type = SOLAR_OS_VKB_ACTION_TOGGLE;
            return true;
        }
        return false;
    }

    /* ---- keyboard visible ---- */
    switch (action) {
    case SOLAR_OS_INPUT_POINTER_PRESS: {
        int r = -1, c = -1;
        if (vkb_key_at(vkb, x, y, &r, &c)) {
            vkb->pointer_down = true;
            vkb->pressed_key_row = r;
            vkb->pressed_key_col = c;
            vkb->rows[r].keys[c].highlighted = true;
        }
        /* consume all presses within keyboard area so the terminal
           doesn't receive them */
        const int kb_top = vkb->display_height - vkb->keyboard_height;
        return y >= kb_top;
    }

    case SOLAR_OS_INPUT_POINTER_MOVE: {
        if (!vkb->pointer_down) {
            return y >= (vkb->display_height - vkb->keyboard_height);
        }
        int r = -1, c = -1;
        if (vkb_key_at(vkb, x, y, &r, &c)) {
            if (r != vkb->pressed_key_row || c != vkb->pressed_key_col) {
                vkb_clear_highlight(vkb);
                vkb->pressed_key_row = r;
                vkb->pressed_key_col = c;
                vkb->rows[r].keys[c].highlighted = true;
            }
        } else {
            /* slid off all keys: clear highlight but keep tracking */
            vkb_clear_highlight(vkb);
        }
        return true;
    }

    case SOLAR_OS_INPUT_POINTER_RELEASE: {
        bool consumed = vkb->pointer_down || y >= (vkb->display_height - vkb->keyboard_height);
        vkb->pointer_down = false;

        if (vkb->pressed_key_row >= 0 && vkb->pressed_key_col >= 0) {
            int r = vkb->pressed_key_row;
            int c = vkb->pressed_key_col;
            solar_os_vkb_key_t *key = &vkb->rows[r].keys[c];

            /* verify finger is still on the same key at release */
            int rr = -1, cc = -1;
            bool still_on_key = vkb_key_at(vkb, x, y, &rr, &cc) &&
                                 rr == r && cc == c;

            vkb_clear_highlight(vkb);

            if (still_on_key) {
                solar_os_vkb_action_type_t kt = key->action.type;
                /* handle internal mode/toggle actions */
                if (kt == SOLAR_OS_VKB_ACTION_TOGGLE) {
                    solar_os_vkb_set_visible(vkb, false);
                    out_action->type = SOLAR_OS_VKB_ACTION_TOGGLE;
                    return true;
                }
                vkb_apply_mode_action(vkb, kt);
                if (kt == SOLAR_OS_VKB_ACTION_SHIFT ||
                    kt == SOLAR_OS_VKB_ACTION_SYMBOL) {
                    out_action->type = kt;
                    return true;
                }
                /* external actions (char, backspace, enter, etc.) */
                *out_action = key->action;
                return true;
            }
            /* released off-key: cancel, no action */
            return consumed;
        }
        /* no key was pressed — still consume if within keyboard area */
        vkb_clear_highlight(vkb);
        return consumed;
    }

    default:
        return false;
    }
}

#endif /* SOLAR_OS_BOARD_HAS_POINTER */
