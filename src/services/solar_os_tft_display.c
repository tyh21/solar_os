#include "solar_os_tft_display.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_board_display.h"
#include "solar_os_buses.h"
#include "solar_os_display.h"
#include "solar_os_log.h"
#include "tft_ili9341.h"

#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ
#define SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ 40000000U
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_MODE
#define SOLAR_OS_BOARD_DISPLAY_SPI_MODE 0
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_INVERT_COLORS
#define SOLAR_OS_BOARD_DISPLAY_INVERT_COLORS 0
#endif

/* Per-controller defaults preserve exact current behavior for boards that
 * do not (yet) declare these in their manifest [defines]; freenove and
 * odroid_go both already declare values that match these defaults. A board
 * whose panel needs different native dimensions, MADCTL, rotation, or a
 * GRAM window offset (e.g. a glass module smaller than the controller's
 * addressable RAM) can override any of them from its manifest. */
#ifndef SOLAR_OS_BOARD_LCD_BACKLIGHT_PULSE_STEPS
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PULSE_STEPS 0U
#endif
#ifndef SOLAR_OS_BOARD_LCD_BACKLIGHT_STEP_PERCENT
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_STEP_PERCENT 0U
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_COL_OFFSET
#define SOLAR_OS_BOARD_DISPLAY_COL_OFFSET 0U
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_ROW_OFFSET
#define SOLAR_OS_BOARD_DISPLAY_ROW_OFFSET 0U
#endif

#if defined(SOLAR_OS_BOARD_PCA9557_I2C_BUS) && defined(SOLAR_OS_BOARD_PCA9557_I2C_ADDR)
static bool pca9557_ready = false;

/* solar_os_bus_i2c_write_reg requires an ACTIVE lease on the I2C bus
 * (pin_ready_bus rejects lease_count == 0), so each write must hold its
 * own acquire/release pair.  The device itself does not bind i2c0, and
 * touch0/audio0 attach later, so without this the writes silently
 * failed with ESP_ERR_INVALID_STATE and CS was never driven low — every
 * panel command went to a deselected panel (all-green logs, black
 * screen).  Errors are reported through ESP_LOGI because the
 * SOLAR_OS_LOG* channel is not routed to the boot console. */
static bool pca9557_write(uint8_t reg, uint8_t value, const char *what) {
    const esp_err_t acquire_ret = solar_os_bus_acquire(
        SOLAR_OS_BOARD_PCA9557_I2C_BUS,
        SOLAR_OS_BUS_PROTOCOL_I2C, "tft_pca9557");
    if (acquire_ret != ESP_OK) {
        ESP_LOGE("tft_disp", "PCA9557 %s: acquire failed: %s",
                 what, esp_err_to_name(acquire_ret));
        return false;
    }
    const uint8_t data[] = {value};
    const esp_err_t err = solar_os_bus_i2c_write_reg(
        SOLAR_OS_BOARD_PCA9557_I2C_BUS, SOLAR_OS_BOARD_PCA9557_I2C_ADDR,
        reg, data, sizeof(data));
    (void)solar_os_bus_release(SOLAR_OS_BOARD_PCA9557_I2C_BUS,
                               SOLAR_OS_BUS_PROTOCOL_I2C, "tft_pca9557");
    if (err != ESP_OK) {
        ESP_LOGE("tft_disp", "PCA9557 %s write (reg 0x%02x val 0x%02x) failed: %s",
                 what, reg, value, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Hold LCD CS high (deselected) during panel creation so the SPI
 * bus is idle.  CS is asserted low before the init sequence to give
 * the panel a clean transaction-start edge.
 * NB: DVP_PWDN (bit2) must be 0 — verified by A/B experiment: every
 * program that lights the panel uses PWDN=0 (vendor demo sets it
 * via a different bit pattern but the net effect is PWDN low; the
 * standalone test program uses 0x03 = CS1|PA_EN1|PWDN0).  Setting
 * PWDN=1 (as solar_os previously did with 0x07/0x06) produces a
 * black screen even though all SPI commands return ESP_OK. */
static void pca9557_prepare(void) {
    if (!pca9557_ready) {
        return;
    }
    /* bit0=LCD_CS high (deselected), bit1=PA_EN on, bit2=DVP_PWDN off. */
    const bool out_ok = pca9557_write(0x01, 0x03, "output");
    /* P0/P1/P2 outputs, upper pins stay inputs. */
    const bool cfg_ok = pca9557_write(0x03, 0xf8, "config");
    ESP_LOGI("tft_disp", "pca9557_prepare: CS=1 PA_EN=1 PWDN=0 out=%s cfg=%s",
             out_ok ? "ok" : "FAIL", cfg_ok ? "ok" : "FAIL");
}

static void pca9557_select_lcd(void) {
    if (!pca9557_ready) {
        ESP_LOGE("tft_disp", "pca9557_select_lcd skipped: not ready");
        return;
    }
    /* Drive LCD CS low, keep PA_EN=1, PWDN=0. */
    const bool ok = pca9557_write(0x01, 0x02, "select");
    ESP_LOGI("tft_disp", "pca9557_select_lcd: CS=0 PA_EN=1 PWDN=0 %s", ok ? "ok" : "FAIL");
}
#endif

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    tft_ili9341_t driver;
    solar_os_board_display_t display;
} tft_device_t;

static tft_device_t *device;

static bool role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t count,
                                char *spi_bus,
                                int *cs,
                                int *dc,
                                int *reset,
                                int *backlight,
                                bool *backlight_active_high,
                                bool *backlight_pwm)
{
    bool have_spi = false;
    bool have_cs = false;
    spi_bus[0] = '\0';
    *cs = -1;
    *dc = -1;
    *reset = -1;
    *backlight = -1;
    *backlight_active_high = true;
    *backlight_pwm = false;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS && !have_spi) {
            strlcpy(spi_bus, binding->target, SOLAR_OS_EXPANSION_TARGET_MAX);
            have_spi = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS && !have_cs) {
            *cs = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(spi_bus, binding->target, SOLAR_OS_EXPANSION_TARGET_MAX);
                have_spi = true;
            }
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   role_is(binding, "dc") && *dc < 0) {
            *dc = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   role_is(binding, "reset") && *reset < 0) {
            *reset = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   role_is(binding, "bl") && *backlight < 0) {
            *backlight = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   role_is(binding, "active")) {
            *backlight_active_high = binding->value != 0;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   role_is(binding, "pwm")) {
            *backlight_pwm = binding->value != 0;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_spi && *dc >= 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL && display->driver != NULL ?
        tft_ili9341_resume(display->driver) : ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        tft_ili9341_deinit(display->driver);
        display->ready = false;
    }
}

static bool brightness_supported(const solar_os_board_display_t *display)
{
    (void)display;
    return tft_ili9341_backlight_supported();
}

static esp_err_t get_brightness(const solar_os_board_display_t *display, uint8_t *percent)
{
    return display != NULL ? tft_ili9341_get_backlight(display->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_brightness(solar_os_board_display_t *display, uint8_t percent)
{
    return display != NULL ? tft_ili9341_set_backlight(display->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_colors(solar_os_board_display_t *display,
                            uint32_t foreground,
                            uint32_t background)
{
    return display != NULL ?
        tft_ili9341_set_colors(display->driver, foreground, background) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_surface(solar_os_board_display_t *display,
                                 const solar_os_display_surface_t *surface)
{
    return display != NULL ?
        tft_ili9341_present_surface(display->driver, surface) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_auxiliary_surface(
    void *context,
    const solar_os_display_surface_t *surface)
{
    return tft_ili9341_present_surface((tft_ili9341_t *)context, surface);
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame)
{
    return display != NULL ?
        tft_ili9341_present_frame(display->driver, frame) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_auxiliary_frame(
    void *context,
    const solar_os_display_raster_t *frame)
{
    return tft_ili9341_present_frame((tft_ili9341_t *)context, frame);
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .brightness_supported = brightness_supported,
    .get_brightness = get_brightness,
    .set_brightness = set_brightness,
    .set_colors = set_colors,
    .present_surface = present_surface,
    .present_frame = present_frame,
};

static esp_err_t register_auxiliary(tft_device_t *attached)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, attached->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, attached->display.driver_name, sizeof(target.driver));
    strlcpy(target.controller, attached->display.controller, sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = attached->display.width;
    target.height = attached->display.height;
    target.ready = true;
    target.brightness_supported = brightness_supported(&attached->display);
    target.surface_formats = attached->display.surface_formats;
    target.frame_formats = attached->display.frame_formats;
    target.preferred_stream_fps = attached->display.preferred_stream_fps;
    target.max_stream_pixels_per_second =
        attached->display.max_stream_pixels_per_second;
    target.u8g2 = attached->display.u8g2;
    target.surface_context = &attached->driver;
    target.present_surface = present_auxiliary_surface;
    target.frame_context = &attached->driver;
    target.present_frame = present_auxiliary_frame;
    return solar_os_display_register_target(&target);
}

static esp_err_t attach_tft(const char *name,
                            const solar_os_expansion_binding_t *bindings,
                            size_t binding_count,
                            int controller)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs = -1;
    int dc = -1;
    int reset = -1;
    int backlight = -1;
    bool active_high = true;
    bool pwm = false;
    const bool st7796 = controller == 1;
    const bool st7789 = controller == 2;
    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       spi_bus,
                                       &cs,
                                       &dc,
                                       &reset,
                                       &backlight,
                                       &active_high,
                                       &pwm),
                        "tft",
                        "invalid bindings");
#if defined(SOLAR_OS_BOARD_PCA9557_I2C_BUS) && defined(SOLAR_OS_BOARD_PCA9557_I2C_ADDR)
    if (cs < 0) {
        /* Probe i2c0 the same lease-checked way the writes need; this
         * both verifies the bus works and registers PCA9557 readiness. */
        esp_err_t pca_err = solar_os_bus_acquire(
            SOLAR_OS_BOARD_PCA9557_I2C_BUS,
            SOLAR_OS_BUS_PROTOCOL_I2C, "tft_pca9557");
        if (pca_err == ESP_OK) {
            solar_os_bus_release(SOLAR_OS_BOARD_PCA9557_I2C_BUS,
                                 SOLAR_OS_BUS_PROTOCOL_I2C,
                                 "tft_pca9557");
            pca9557_ready = true;
            /* LCD CS lives on the PCA9557, so the SPI driver has no GPIO to
             * assert it. Prepare outputs with CS deselected; it is asserted
             * between panel_reset and panel_init below. */
            pca9557_prepare();
            cs = GPIO_NUM_NC;
        } else {
            ESP_LOGE("tft_disp", "PCA9557 bus acquire failed: %s",
                     esp_err_to_name(pca_err));
        }
    }
#endif
    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));

    bool panel_pre_inited = false;
    esp_lcd_panel_io_handle_t io_handle = NULL;
#if defined(SOLAR_OS_BOARD_SPI_HOST)
    if (st7789 && reset < 0) {
        /* The SPI bus is initialized by the bus framework (first lease)
         * with the PCA9557 guard already holding LCD CS deselected, so
         * no panel-corrupting edges can leak during spi_bus_initialize. */
        const esp_lcd_panel_io_spi_config_t io_config = {
            .dc_gpio_num = dc,
            .cs_gpio_num = GPIO_NUM_NC,
            .pclk_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .spi_mode = SOLAR_OS_BOARD_DISPLAY_SPI_MODE,
            .trans_queue_depth = 10,
        };
        const esp_err_t io_err = esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)SOLAR_OS_BOARD_SPI_HOST,
            &io_config, &io_handle);
        ESP_LOGI("tft_disp", "esp_lcd io_spi: %s io_handle=%p", esp_err_to_name(io_err), io_handle);
        if (io_err == ESP_OK) {
            /* ST7789 panel via the IDF esp_lcd driver: io handle carries
             * dc/NC-cs/config, panel handle provides swap_xy/mirror/
             * draw_bitmap.  The panel is put through the proven init
             * sequence below (see "Proven fix" comment). */
            esp_lcd_panel_handle_t panel_handle = NULL;
            const esp_lcd_panel_dev_config_t panel_config = {
                .reset_gpio_num = GPIO_NUM_NC,
                .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                .bits_per_pixel = 16,
            };
            const esp_err_t panel_err = esp_lcd_new_panel_st7789(
                io_handle, &panel_config, &panel_handle);
            ESP_LOGI("tft_disp", "esp_lcd st7789: %s", esp_err_to_name(panel_err));
            if (panel_err == ESP_OK) {
                /* --- Proven fix (experiments 2-5, 2026-09-18) ---
                 *
                 * ROOT CAUSE: the SPI peripheral's FIRST transaction after
                 * spi_bus_initialize produces glitch edges on SCLK (clock
                 * gating / pin takeover).  On this board the panel CS line
                 * lives on the I2C expander PCA9557, so the SPI driver has
                 * no way to gate it: if the first transaction lands on a
                 * SELECTED panel (CS=0), the panel's command parser
                 * desynchronizes and silently swallows every subsequent
                 * command — SWRESET, full register rescue and even a
                 * complete re-init all "succeed" (ESP_OK) but the screen
                 * stays black until the panel is power-cycled.
                 *
                 * FIX: burn the first, glitchy transaction while CS is
                 * still deselected (a "sacrificial dummy").  With no RST
                 * GPIO, esp_lcd_panel_reset() = tx_param(SWRESET)+20ms,
                 * which the panel cannot see at CS=1 — it only warms up
                 * the SPI peripheral.  Then select the panel and start
                 * the real init; every real command goes out clean.
                 *
                 * Evidence: exp2/exp4 (first tx at CS=0) = black hot AND
                 * cold; exp5 (this sequence) = bright hot AND cold. */
                esp_lcd_panel_reset(panel_handle); /* dummy tx at CS=1: absorbs glitch */
                ESP_LOGI("tft_disp", "sacrificial dummy tx sent at CS=1");
#if defined(SOLAR_OS_BOARD_PCA9557_I2C_BUS) && defined(SOLAR_OS_BOARD_PCA9557_I2C_ADDR)
                pca9557_select_lcd(); /* CS high->low: panel selected for real init */
#endif

                esp_lcd_panel_init(panel_handle);
                /* Invert only — do NOT swap_xy/mirror here.  The
                 * tft_ili9341 render path does rotation in SOFTWARE
                 * (surface ROTATION_90 / u8g2 U8G2_R1) and expects the
                 * panel in its NATIVE PORTRAIT orientation (MADCTL 0x00,
                 * as declared by the board manifest).  Adding hardware
                 * rotation here double-rotates the UI by 90 degrees and
                 * makes window writes addressed for a 320-row axis land
                 * on a 240-row axis — the top 80 rows go out of range
                 * and show as garbage stripes. */
                esp_lcd_panel_invert_color(panel_handle, true);
                esp_lcd_panel_disp_on_off(panel_handle, true);
                ESP_LOGI("tft_disp", "panel init done (native portrait, SW rotation)");

                panel_pre_inited = true;
                SOLAR_OS_LOGI("tft", "esp_lcd panel initialized");
            } else {
                SOLAR_OS_LOGE("tft", "esp_lcd_new_panel_st7789 failed: %s",
                              esp_err_to_name(panel_err));
            }
            /* The esp_lcd panel_handle is intentionally NOT deleted:
             * tft_ili9341 keeps using the io_handle (panel_io) for all
             * subsequent tx_param/tx_color traffic, and the IDF panel
             * struct holds only the ST7789 MADCTL state we no longer
             * need after init.  It is a small one-time allocation that
             * lives for the lifetime of the attached device. */
         } else {
            SOLAR_OS_LOGE("tft", "esp_lcd_new_panel_io_spi failed: %s",
                          esp_err_to_name(io_err));
        }
    }
#endif

    const tft_ili9341_config_t config = {
        .spi_bus = device->spi_bus,
        .cs_pin = cs,
        .dc_pin = dc,
        .reset_pin = reset,
        .backlight_pin = backlight,
        .spi_clock_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
        .backlight_pwm_hz = 20000U,
#if defined(SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH) && defined(SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT)
        .width = SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH,
        .height = SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT,
#else
        .width = (st7796 || st7789) ? 320 : 240,
        .height = (st7796 || st7789) ? 480 : 320,
#endif
#ifdef SOLAR_OS_BOARD_DISPLAY_MADCTL
        .madctl = SOLAR_OS_BOARD_DISPLAY_MADCTL,
#else
        .madctl = (st7796 || st7789) ? 0x48 : 0x88,
#endif
        .col_offset = SOLAR_OS_BOARD_DISPLAY_COL_OFFSET,
        .row_offset = SOLAR_OS_BOARD_DISPLAY_ROW_OFFSET,
        .st7796 = st7796,
        .st7789 = st7789,
        .spi_mode = SOLAR_OS_BOARD_DISPLAY_SPI_MODE,
        .invert_colors = SOLAR_OS_BOARD_DISPLAY_INVERT_COLORS,
        .backlight_active_high = active_high,
        .backlight_pwm = pwm,
        .panel_pre_inited = panel_pre_inited,
        .panel_io_handle = panel_pre_inited ? io_handle : NULL,
        .backlight_pulse_steps = SOLAR_OS_BOARD_LCD_BACKLIGHT_PULSE_STEPS,
        .backlight_step_percent = SOLAR_OS_BOARD_LCD_BACKLIGHT_STEP_PERCENT,
#ifdef SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION
        .rotation = SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION,
#else
        .rotation = U8G2_R1,
#endif
    };
    esp_err_t ret = tft_ili9341_init(&device->driver, &config);
    ESP_LOGI("tft_disp", "init: %s panel_io=%p spi=%p",
             esp_err_to_name(ret), device->driver.panel_io, device->driver.spi);
    if (ret != ESP_OK) {
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    strlcpy(device->name, name, sizeof(device->name));
    u8g2_t *const u8g2 = tft_ili9341_get_u8g2(&device->driver);
    const char *driver_name = st7789 ? "st7789" : (st7796 ? "st7796" : "ili9341");
    const char *controller_name = st7789 ? "ST7789" : (st7796 ? "ST7796" : "ILI9341");
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = driver_name,
        .u8g2 = u8g2,
        .controller = controller_name,
        .width = u8g2_GetDisplayWidth(u8g2),
        .height = u8g2_GetDisplayHeight(u8g2),
        .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT,
        .preferred_stream_fps = (st7796 || st7789) ? 25 : 30,
        .max_stream_pixels_per_second = 1600000U,
        .ready = true,
    };
    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    ret = device->primary ?
        solar_os_board_display_register_primary(&device->display) :
        register_auxiliary(device);
    if (ret != ESP_OK) {
        tft_ili9341_deinit(&device->driver);
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    device->active = true;
    return ESP_OK;
}

static esp_err_t attach_ili9341(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    return attach_tft(name, bindings, binding_count, 0);
}

static esp_err_t attach_st7796(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    return attach_tft(name, bindings, binding_count, 1);
}

static esp_err_t attach_st7789(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    return attach_tft(name, bindings, binding_count, 2);
}

static esp_err_t detach(const char *name)
{
    if (device == NULL || !device->active || name == NULL ||
        strcmp(device->name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = device->primary ?
        solar_os_board_display_unregister_primary(&device->display) :
        solar_os_display_unregister_target(name);
    if (ret != ESP_OK) {
        return ret;
    }
    tft_ili9341_deinit(&device->driver);
    heap_caps_free(device);
    device = NULL;
    return ESP_OK;
}

static const int bool_values[] = {0, 1};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = false},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
    {.key = "bl", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bl"},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .allowed_values = bool_values, .allowed_value_count = 2},
    {.key = "pwm", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pwm", .allowed_values = bool_values, .allowed_value_count = 2},
};

#define TFT_CAPABILITIES (SOLAR_OS_BOARD_CAP_GFX | \
                          SOLAR_OS_BOARD_CAP_EXPANSION_SPI | \
                          SOLAR_OS_BOARD_CAP_EXPANSION_GPIO)

const solar_os_expansion_driver_t solar_os_ili9341_expansion_driver = {
    .name = "ili9341",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "320x240 color TFT",
    .required_capabilities = TFT_CAPABILITIES,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_ili9341,
    .detach = detach,
};

const solar_os_expansion_driver_t solar_os_st7796_expansion_driver = {
    .name = "st7796",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "480x320 color TFT",
    .required_capabilities = TFT_CAPABILITIES,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_st7796,
    .detach = detach,
};

const solar_os_expansion_driver_t solar_os_st7789_expansion_driver = {
    .name = "st7789",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "320x240 color TFT",
    .required_capabilities = TFT_CAPABILITIES,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_st7789,
    .detach = detach,
};
