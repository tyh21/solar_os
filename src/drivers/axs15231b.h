#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sclk_pin;
    int data0_pin;
    int data1_pin;
    int data2_pin;
    int data3_pin;
} axs15231b_bus_pins_t;

/* Installs the quad-mode SPI bus for the AXS15231B panel. The bus is owned
 * exclusively by the display driver: SolarOS buses cannot express quad data
 * lines, so this bus never registers with the shared bus service. */
esp_err_t axs15231b_bus_init(spi_host_device_t host,
                             const axs15231b_bus_pins_t *pins,
                             size_t max_transfer_sz);

/* Creates the quad-mode esp_lcd panel IO (32-bit command frames, 8-bit
 * single-wire parameters, quad pixel data). */
esp_err_t axs15231b_panel_io_create(spi_host_device_t host,
                                    int cs_pin,
                                    uint32_t clock_hz,
                                    esp_lcd_panel_io_handle_t *panel_io);

/* Software reset and the vendor initialization sequence for the Waveshare
 * 3.5" (320x480) AXS15231B glass. The LCD reset line itself is wired to an
 * I/O expander and handled by the board core driver before this runs. */
esp_err_t axs15231b_panel_init_sequence(esp_lcd_panel_io_handle_t panel_io);

void axs15231b_bus_free(spi_host_device_t host,
                        esp_lcd_panel_io_handle_t panel_io);

#ifdef __cplusplus
}
#endif
