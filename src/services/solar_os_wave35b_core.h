#pragma once

#include <stddef.h>

#include "solar_os_expansion.h"

/* Board bring-up for the Waveshare ESP32-S3-Touch-LCD-3.5B: TCA9554 IO
 * expander (panel reset pulse) followed by the AXP2101 PMIC rail setup.
 * Matches the xiaozhi/MicroPixel reference sequence proven to light the
 * panel. Must be listed before the display device in the board manifest. */

#define SOLAR_OS_WAVE35B_CORE_PMIC_ADDRESS 0x34U
#define SOLAR_OS_WAVE35B_CORE_EXPANDER_ADDRESS 0x20U

esp_err_t solar_os_wave35b_core_attach(const char *name,
                                       const solar_os_expansion_binding_t *bindings,
                                       size_t binding_count);
esp_err_t solar_os_wave35b_core_detach(const char *name);
