#include "solar_os_wave35b_core.h"

static const int pmic_addresses[] = {SOLAR_OS_WAVE35B_CORE_PMIC_ADDRESS};
static const int expander_addresses[] = {SOLAR_OS_WAVE35B_CORE_EXPANDER_ADDRESS};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "pmic", .value_hint = "0x34", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = pmic_addresses, .allowed_value_count = sizeof(pmic_addresses) / sizeof(pmic_addresses[0])},
    {.key = "expander", .value_hint = "0x20", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .allowed_values = expander_addresses, .allowed_value_count = sizeof(expander_addresses) / sizeof(expander_addresses[0])},
};

const solar_os_expansion_driver_t solar_os_wave35b_core_expansion_driver = {
    .name = "wave35b-core",
    .category = SOLAR_OS_EXPANSION_CATEGORY_POWER,
    .summary = "Waveshare 3.5B power sequencer",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported = true,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = solar_os_wave35b_core_attach,
    .detach = solar_os_wave35b_core_detach,
};
