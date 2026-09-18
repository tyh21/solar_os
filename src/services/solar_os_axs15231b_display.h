#pragma once

#include <stddef.h>

#include "solar_os_expansion.h"

esp_err_t solar_os_axs15231b_display_attach(const char *name,
                                            const solar_os_expansion_binding_t *bindings,
                                            size_t binding_count);
esp_err_t solar_os_axs15231b_display_detach(const char *name);

extern const solar_os_expansion_driver_t solar_os_axs15231b_display_expansion_driver;
