#include "solar_os_axs15231b_display.h"

static const int bool_values[] = {0, 1};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "cs", .required = true},
    {.key = "bl", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bl"},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .allowed_values = bool_values, .allowed_value_count = 2},
    {.key = "pwm", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pwm", .allowed_values = bool_values, .allowed_value_count = 2},
};

#define AXS_CAPABILITIES (SOLAR_OS_BOARD_CAP_GPIO | \
                          SOLAR_OS_BOARD_CAP_PWM)

const solar_os_expansion_driver_t solar_os_axs15231b_display_expansion_driver = {
    .name = "axs15231b",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "320x480 QSPI color TFT",
    .required_capabilities = AXS_CAPABILITIES,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_axs15231b_display_attach,
    .detach = solar_os_axs15231b_display_detach,
};
