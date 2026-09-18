#include "solar_os_axs15231b_touch.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "solar_os_buses.h"
#include "solar_os_display.h"
#include "solar_os_input.h"
#include "solar_os_log.h"

/*
 * The AXS15231B touch controller shares the panel's custom I2C protocol:
 * write the 11-byte request header, then read back a packed point record.
 * Frame layout matches the Waveshare reference (01_fac bsp_touch.c):
 *   data[0] gesture, data[1] point count,
 *   point n: data[6n+2] x_high(nibble), data[6n+3] x_low,
 *            data[6n+4] y_high(nibble), data[6n+5] y_low.
 */

#define AXS15231B_TOUCH_ADDRESS 0x3BU
#define AXS15231B_TOUCH_POINTS_MAX 2U
#define AXS15231B_TOUCH_REQUEST_SIZE 11U
#define AXS15231B_TOUCH_REPLY_SIZE 14U

static const uint8_t touch_request[AXS15231B_TOUCH_REQUEST_SIZE] = {
    0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00};

typedef struct {
    bool active;
    bool pressed;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    uint8_t rotation;
    uint16_t target_width;
    uint16_t target_height;
    uint8_t pointer_id;
    int16_t x;
    int16_t y;
    solar_os_input_source_t input_source;
} solar_os_axs15231b_touch_device_t;

static const char *TAG = "axs15231b-touch";
static solar_os_axs15231b_touch_device_t touch;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                solar_os_axs15231b_touch_device_t *device)
{
    bool have_i2c = false;
    bool have_address = false;
    bool have_rotation = false;

    if (bindings == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(device->i2c_bus, binding->target, sizeof(device->i2c_bus));
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address || binding->value != AXS15231B_TOUCH_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            device->address = (uint8_t)binding->value;
            have_address = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
            if (strcmp(binding->role, "rotation") != 0 || have_rotation ||
                binding->value < 0 || binding->value > 3) {
                return ESP_ERR_INVALID_ARG;
            }
            device->rotation = (uint8_t)binding->value;
            have_rotation = true;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address && have_rotation
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static void clear_device(void)
{
    if (touch.input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(touch.input_source);
    }
    memset(&touch, 0, sizeof(touch));
}

esp_err_t solar_os_axs15231b_touch_attach(const char *name,
                                          const solar_os_expansion_binding_t *bindings,
                                          size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (touch.active) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_axs15231b_touch_device_t candidate = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &candidate),
                        TAG, "invalid bindings");
    solar_os_display_target_t target;
    if (!solar_os_display_find_target(SOLAR_OS_DISPLAY_PRIMARY_TARGET, &target) ||
        target.width == 0 || target.height == 0) {
        SOLAR_OS_LOGE(TAG, "primary display target not found");
        return ESP_ERR_NOT_FOUND;
    }
    candidate.target_width = target.width;
    candidate.target_height = target.height;
    const esp_err_t probe_err =
        solar_os_bus_i2c_probe(candidate.i2c_bus, candidate.address);
    if (probe_err != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "touch controller 0x%02x probe failed: %s",
                      (unsigned)candidate.address, esp_err_to_name(probe_err));
        return probe_err;
    }

    strlcpy(candidate.name, name, sizeof(candidate.name));
    esp_err_t err = solar_os_input_touch_source_open(candidate.name,
                                                     &candidate.input_source);
    if (err != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "input touch source open failed: %s",
                      esp_err_to_name(err));
        return err;
    }
    candidate.active = true;
    touch = candidate;
    SOLAR_OS_LOGI(TAG, "%s attached: AXS15231B touch on %s rotation=%u",
                  name, candidate.i2c_bus, (unsigned)candidate.rotation);
    return ESP_OK;
}

esp_err_t solar_os_axs15231b_touch_detach(const char *name)
{
    if (!touch.active || name == NULL || strcmp(touch.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    clear_device();
    return ESP_OK;
}

void solar_os_axs15231b_touch_poll(void)
{
    if (!touch.active) {
        return;
    }

    uint8_t data[AXS15231B_TOUCH_REPLY_SIZE] = {0};
    if (solar_os_bus_i2c_transmit_receive(touch.i2c_bus,
                                          touch.address,
                                          touch_request,
                                          sizeof(touch_request),
                                          data,
                                          sizeof(data)) != ESP_OK) {
        return;
    }

    bool touched = false;
    uint16_t sample_x = 0;
    uint16_t sample_y = 0;
    const uint8_t point_count = data[1];
    if (point_count > 0 && point_count <= AXS15231B_TOUCH_POINTS_MAX &&
        data[2] != 0 && data[3] >= 2 && data[5] >= 2) {
        sample_x = (uint16_t)(((uint16_t)(data[2] & 0x0FU) << 8) | data[3]);
        sample_y = (uint16_t)(((uint16_t)(data[4] & 0x0FU) << 8) | data[5]);
        touched = true;
    }

    uint16_t mapped_x = 0;
    uint16_t mapped_y = 0;
    if (touched) {
        const uint16_t native_width = (touch.rotation & 1U) != 0U
            ? touch.target_height : touch.target_width;
        const uint16_t native_height = (touch.rotation & 1U) != 0U
            ? touch.target_width : touch.target_height;
        if (sample_x >= native_width || sample_y >= native_height) {
            return;
        }
        switch (touch.rotation) {
        case 0:
            mapped_x = sample_x;
            mapped_y = sample_y;
            break;
        case 1:
            mapped_x = sample_y;
            mapped_y = (native_width - 1U) - sample_x;
            break;
        case 2:
            mapped_x = (native_width - 1U) - sample_x;
            mapped_y = (native_height - 1U) - sample_y;
            break;
        default:
            mapped_x = (native_height - 1U) - sample_y;
            mapped_y = sample_x;
            break;
        }
    }

    solar_os_input_pointer_action_t action;
    if (touched && !touch.pressed) {
        action = SOLAR_OS_INPUT_POINTER_PRESS;
    } else if (!touched && touch.pressed) {
        action = SOLAR_OS_INPUT_POINTER_RELEASE;
    } else if (touched && (mapped_x != (uint16_t)touch.x ||
                           mapped_y != (uint16_t)touch.y)) {
        action = SOLAR_OS_INPUT_POINTER_MOVE;
    } else {
        return;
    }

    const int16_t next_x = touched ? (int16_t)mapped_x : touch.x;
    const int16_t next_y = touched ? (int16_t)mapped_y : touch.y;
    solar_os_input_pointer_event_t event = {
        .pointer_id = touch.pointer_id,
        .buttons = touched ? SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY : 0,
        .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = action,
        .x = next_x,
        .y = next_y,
        .delta_x = (int16_t)(next_x - touch.x),
        .delta_y = (int16_t)(next_y - touch.y),
    };
    strlcpy(event.target,
            SOLAR_OS_DISPLAY_PRIMARY_TARGET,
            sizeof(event.target));
    if (solar_os_input_write_pointer(touch.input_source, &event) == ESP_OK) {
        touch.pressed = touched;
        touch.x = next_x;
        touch.y = next_y;
    }
}
