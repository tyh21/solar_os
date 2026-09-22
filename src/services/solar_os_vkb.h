#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "solar_os_input.h"
#include "solar_os_ime.h"

/*
 * On-screen virtual keyboard for touch-equipped SolarOS boards.
 *
 * The keyboard is drawn as an overlay at the bottom of the terminal area.
 * A small trigger toggle ("KB") sits at the bottom-right corner; tapping it
 * shows or hides the full keyboard.  When visible, the terminal shrinks its
 * text rows to make room, and touch events on the keyboard surface are
 * translated into character/key actions that are fed back to the shell.
 *
 * The module is self-contained: callers provide the u8g2 context, display
 * dimensions, and touch events; the vkb owns its state, layout, and
 * rendering.  Compilation is guarded by SOLAR_OS_BOARD_HAS_POINTER so that
 * boards without touch hardware are unaffected.
 */

/* Maximum number of keys per row. */
#define SOLAR_OS_VKB_MAX_ROW_KEYS 16

/* Keyboard height is 4/15 of the display height (rounded to 4 rows). */
#define SOLAR_OS_VKB_ROW_COUNT 4

typedef enum {
    SOLAR_OS_VKB_MODE_LOWER,   /* lowercase letters + minimal punctuation */
    SOLAR_OS_VKB_MODE_UPPER,   /* uppercase letters (shift toggle)        */
    SOLAR_OS_VKB_MODE_SYMBOL,  /* digits + symbols                        */
    SOLAR_OS_VKB_MODE_PINYIN,  /* pinyin IME: candidate row + letters     */
} solar_os_vkb_mode_t;

typedef enum {
    SOLAR_OS_VKB_ACTION_NONE,
    SOLAR_OS_VKB_ACTION_CHAR,      /* emit a single character */
    SOLAR_OS_VKB_ACTION_BACKSPACE, /* delete previous char    */
    SOLAR_OS_VKB_ACTION_ENTER,     /* submit line             */
    SOLAR_OS_VKB_ACTION_SPACE,     /* space                   */
    SOLAR_OS_VKB_ACTION_LEFT,      /* cursor left             */
    SOLAR_OS_VKB_ACTION_RIGHT,     /* cursor right            */
    SOLAR_OS_VKB_ACTION_SHIFT,     /* toggle upper/lower      */
    SOLAR_OS_VKB_ACTION_SYMBOL,    /* toggle symbol mode      */
    SOLAR_OS_VKB_ACTION_TOGGLE,   /* hide keyboard           */
    SOLAR_OS_VKB_ACTION_IME_SELECT, /* choose IME candidate; ch = index */
    SOLAR_OS_VKB_ACTION_IME_ABC,   /* leave pinyin mode, back to letters */
    SOLAR_OS_VKB_ACTION_IME_PAGE_PREV, /* previous candidate page */
    SOLAR_OS_VKB_ACTION_IME_PAGE_NEXT, /* next candidate page */
} solar_os_vkb_action_type_t;

typedef struct {
    solar_os_vkb_action_type_t type;
    char ch;                     /* valid when type == ACTION_CHAR /
                                    ACTION_IME_SELECT (index) */
} solar_os_vkb_action_t;

typedef struct {
    int x, y, w, h;              /* pixel rect of the key           */
    const char *label;          /* display label (1-3 chars)        */
    solar_os_vkb_action_t action;/* what this key does              */
    bool highlighted;           /* currently pressed (preview)      */
} solar_os_vkb_key_t;

typedef struct {
    solar_os_vkb_key_t keys[SOLAR_OS_VKB_MAX_ROW_KEYS];
    int count;
    int y_top;                  /* top-y of this row in pixels      */
} solar_os_vkb_row_t;

typedef struct {
    bool visible;
    bool pointer_down;          /* a touch is currently active      */
    int  pointer_x;             /* last touch x                     */
    int  pointer_y;             /* last touch y                     */
    int  pressed_key_row;       /* -1 when no key is pressed        */
    int  pressed_key_col;       /* index within the row             */
    solar_os_vkb_mode_t mode;
    solar_os_vkb_mode_t prev_mode; /* restore from shift after upper  */
    solar_os_vkb_row_t rows[SOLAR_OS_VKB_ROW_COUNT];
    int keyboard_height;        /* total height in pixels           */
    int display_width;
    int display_height;
    int trigger_x, trigger_y, trigger_w, trigger_h;
    bool layout_valid;
    /* Pinyin IME candidate row (PINYIN mode).  Each entry is a
     * NUL-terminated UTF-8 string owned by the caller; count is the
     * number of valid entries (<= SOLAR_OS_IME_PAGE_SIZE). */
    const char *ime_candidates[SOLAR_OS_IME_PAGE_SIZE];
    int ime_candidate_count;
} solar_os_vkb_t;

/* Initialise the virtual keyboard state. */
void solar_os_vkb_init(solar_os_vkb_t *vkb);

/* Return true when the keyboard is currently visible. */
bool solar_os_vkb_is_visible(const solar_os_vkb_t *vkb);

/* Show or hide the keyboard.  Returns true if visibility changed. */
bool solar_os_vkb_set_visible(solar_os_vkb_t *vkb, bool visible);

/* Toggle visibility.  Returns the new visible state. */
bool solar_os_vkb_toggle(solar_os_vkb_t *vkb);

/*
 * Compute the keyboard overlay height in pixels for the given display
 * height.  When the keyboard is visible the terminal should subtract
 * this from its content area so text rows shrink accordingly.
 */
int solar_os_vkb_overlay_height(const solar_os_vkb_t *vkb, int display_height);

/*
 * Recalculate key positions for the current display dimensions.  Called
 * automatically from draw and handle_pointer when layout_valid is false.
 */
void solar_os_vkb_layout(solar_os_vkb_t *vkb, int display_width, int display_height);

/*
 * Draw the keyboard (and the trigger toggle when hidden) onto the u8g2
 * buffer.  palette_inverted and black_is_one control the draw-color
 * polarity exactly as in terminal_set_draw_color().
 */
void solar_os_vkb_draw(solar_os_vkb_t *vkb,
                       void *u8g2_ptr,
                       bool palette_inverted,
                       bool black_is_one);

/*
 * Feed a pointer event to the keyboard.  When the event hits the trigger
 * or a key, the resulting action is written to *out_action and the function
 * returns true.  When the event misses everything (or the keyboard is
 * hidden and the touch is not on the trigger) it returns false so the
 * caller can decide whether to pass it through.
 *
 * The x/y coordinates must already be oriented to match the display
 * buffer (the same orientation the terminal uses for drawing).
 */
bool solar_os_vkb_handle_pointer(solar_os_vkb_t *vkb,
                                 solar_os_input_pointer_action_t action,
                                 int x,
                                 int y,
                                 solar_os_vkb_action_t *out_action);

/* Return the trigger toggle rect for testing by external code. */
void solar_os_vkb_trigger_rect(const solar_os_vkb_t *vkb,
                               int *x, int *y, int *w, int *h);

/*
 * Switch the keyboard to an explicit mode.  Used by the shell to enter
 * (and leave) the pinyin IME layout.
 */
void solar_os_vkb_set_mode(solar_os_vkb_t *vkb, solar_os_vkb_mode_t mode);

/*
 * Feed the pinyin candidate row.  candidates may be NULL to clear.
 * Each string must stay valid until the next call or until the vkb
 * leaves PINYIN mode (the shell owns the buffers).
 */
void solar_os_vkb_set_candidates(solar_os_vkb_t *vkb,
                                 const char *const *candidates,
                                 int count);

/* Number of visible candidates (0 when not in PINYIN mode). */
int solar_os_vkb_candidate_count(const solar_os_vkb_t *vkb);
