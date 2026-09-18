#include "solar_os_axs15231b_display.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "solar_os_board.h"
#include "axs15231b.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"
#include "solar_os_log.h"
#include "tft_ili9341.h"

#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_HOST
#define SOLAR_OS_BOARD_DISPLAY_SPI_HOST SPI2_HOST
#endif
/* Boards that do not carry the Waveshare 3.5B QSPI panel define none of the
 * LCD pins; the bus initializer rejects the all-NC configuration so the
 * expansion device simply fails to attach there. */
#ifndef SOLAR_OS_BOARD_PIN_LCD_SCLK
#define SOLAR_OS_BOARD_PIN_LCD_SCLK (-1)
#endif
#ifndef SOLAR_OS_BOARD_PIN_LCD_DATA0
#define SOLAR_OS_BOARD_PIN_LCD_DATA0 (-1)
#endif
#ifndef SOLAR_OS_BOARD_PIN_LCD_DATA1
#define SOLAR_OS_BOARD_PIN_LCD_DATA1 (-1)
#endif
#ifndef SOLAR_OS_BOARD_PIN_LCD_DATA2
#define SOLAR_OS_BOARD_PIN_LCD_DATA2 (-1)
#endif
#ifndef SOLAR_OS_BOARD_PIN_LCD_DATA3
#define SOLAR_OS_BOARD_PIN_LCD_DATA3 (-1)
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 320
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 480
#endif
#ifndef SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R0
#endif

#ifndef SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ
#define SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ 40000000U
#endif

#ifndef SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ 25000U
#endif

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    tft_ili9341_t driver;
    solar_os_board_display_t display;
    bool bus_ready;
    esp_lcd_panel_io_handle_t panel_io;
} axs_display_device_t;

static const char *TAG = "axs15231b";
static axs_display_device_t *device;

static bool role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t count,
                                int *cs,
                                int *backlight,
                                bool *backlight_active_high,
                                bool *backlight_pwm)
{
    bool have_cs = false;
    bool have_bl = false;
    *cs = -1;
    *backlight = -1;
    *backlight_active_high = true;
    *backlight_pwm = false;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
            role_is(binding, "cs") && *cs < 0) {
            *cs = binding->value;
            have_cs = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   role_is(binding, "bl") && *backlight < 0) {
            *backlight = binding->value;
            have_bl = true;
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
    return have_cs ? ESP_OK : ESP_ERR_INVALID_ARG;
    (void)have_bl;
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL && display->driver != NULL
        ? tft_ili9341_resume(display->driver) : ESP_ERR_INVALID_STATE;
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
    return display != NULL ? tft_ili9341_get_backlight(display->driver, percent)
        : ESP_ERR_INVALID_STATE;
}

static esp_err_t set_brightness(solar_os_board_display_t *display, uint8_t percent)
{
    return display != NULL ? tft_ili9341_set_backlight(display->driver, percent)
        : ESP_ERR_INVALID_STATE;
}

static esp_err_t set_colors(solar_os_board_display_t *display,
                            uint32_t foreground,
                            uint32_t background)
{
    return display != NULL
        ? tft_ili9341_set_colors(display->driver, foreground, background)
        : ESP_ERR_INVALID_STATE;
}

static esp_err_t present_surface(solar_os_board_display_t *display,
                                 const solar_os_display_surface_t *surface)
{
    return display != NULL
        ? tft_ili9341_present_surface(display->driver, surface)
        : ESP_ERR_INVALID_STATE;
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame)
{
    return display != NULL
        ? tft_ili9341_present_frame(display->driver, frame)
        : ESP_ERR_INVALID_STATE;
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

static void detach_device(axs_display_device_t *attached)
{
    tft_ili9341_deinit(&attached->driver);
    axs15231b_bus_free(SOLAR_OS_BOARD_DISPLAY_SPI_HOST, attached->panel_io);
    attached->panel_io = NULL;
    attached->bus_ready = false;
    heap_caps_free(attached);
}

esp_err_t solar_os_axs15231b_display_attach(const char *name,
                                            const solar_os_expansion_binding_t *bindings,
                                            size_t binding_count)
{
    int cs = -1;
    int backlight = -1;
    bool active_high = true;
    bool pwm = false;

    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &cs, &backlight,
                                       &active_high, &pwm),
                        TAG, "invalid bindings");

    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t max_transfer_sz =
        (size_t)SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH * 2U * 8U;
    const axs15231b_bus_pins_t pins = {
        .sclk_pin = SOLAR_OS_BOARD_PIN_LCD_SCLK,
        .data0_pin = SOLAR_OS_BOARD_PIN_LCD_DATA0,
        .data1_pin = SOLAR_OS_BOARD_PIN_LCD_DATA1,
        .data2_pin = SOLAR_OS_BOARD_PIN_LCD_DATA2,
        .data3_pin = SOLAR_OS_BOARD_PIN_LCD_DATA3,
    };
    esp_err_t ret = axs15231b_bus_init(SOLAR_OS_BOARD_DISPLAY_SPI_HOST, &pins,
                                       max_transfer_sz);
    if (ret != ESP_OK) {
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    device->bus_ready = true;

    ret = axs15231b_panel_io_create(SOLAR_OS_BOARD_DISPLAY_SPI_HOST, cs,
                                    SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
                                    &device->panel_io);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "panel io create failed: %s", esp_err_to_name(ret));
        detach_device(device);
        device = NULL;
        return ret;
    }
    ret = axs15231b_panel_init_sequence(device->panel_io);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "panel init sequence failed: %s", esp_err_to_name(ret));
        detach_device(device);
        device = NULL;
        return ret;
    }

    const tft_ili9341_config_t config = {
        .spi_bus = NULL,
        .cs_pin = -1,
        .dc_pin = -1,
        .reset_pin = -1,
        .backlight_pin = backlight,
        .spi_clock_hz = SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ,
        .backlight_pwm_hz = SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ,
        .width = SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH,
        .height = SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT,
        .axs15231b = true,
        .invert_colors = false,
        .backlight_active_high = active_high,
        .backlight_pwm = pwm,
        .panel_pre_inited = true,
        .panel_io_handle = device->panel_io,
        .rotation = SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION,
    };
    ret = tft_ili9341_init(&device->driver, &config);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "display init failed: %s", esp_err_to_name(ret));
        detach_device(device);
        device = NULL;
        return ret;
    }

    strlcpy(device->name, name, sizeof(device->name));
    u8g2_t *const u8g2 = tft_ili9341_get_u8g2(&device->driver);
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "axs15231b",
        .u8g2 = u8g2,
        .controller = "AXS15231B",
        .width = u8g2_GetDisplayWidth(u8g2),
        .height = u8g2_GetDisplayHeight(u8g2),
        .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT,
        .preferred_stream_fps = 25,
        .max_stream_pixels_per_second = 1600000U,
        .ready = true,
    };
    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    ret = device->primary
        ? solar_os_board_display_register_primary(&device->display)
        : ESP_ERR_NOT_SUPPORTED;
    if (ret != ESP_OK) {
        tft_ili9341_deinit(&device->driver);
        axs15231b_bus_free(SOLAR_OS_BOARD_DISPLAY_SPI_HOST, device->panel_io);
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    device->active = true;
    SOLAR_OS_LOGI(TAG, "%s attached: %ux%u QSPI panel ready",
                  name, (unsigned)device->display.width,
                  (unsigned)device->display.height);
    return ESP_OK;
}

esp_err_t solar_os_axs15231b_display_detach(const char *name)
{
    if (device == NULL || !device->active || name == NULL ||
        strcmp(device->name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = device->primary
        ? solar_os_board_display_unregister_primary(&device->display)
        : ESP_ERR_NOT_FOUND;
    if (ret != ESP_OK) {
        return ret;
    }
    device->active = false;
    detach_device(device);
    device = NULL;
    return ESP_OK;
}
