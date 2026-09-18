/* EXPERIMENT 5: THE SACRIFICIAL DUMMY TRANSACTION.
 *
 * Base = EXPERIMENT 4 (BLACK): guard -> SPI init -> io -> panel
 *        -> CS=0 -> panel_init (first bus transaction lands on selected panel) -> black.
 *
 * ONLY CHANGE: while CS is still 1 (deselected), issue ONE dummy SPI
 * transaction (SWRESET via esp_lcd_panel_reset) BEFORE pulling CS=0.
 * This absorbs the first-transaction glitch, exactly like golden reference.
 * No psram_dma_direct flag (keeps exp4 config, isolates the single variable).
 *
 * Outcomes:
 *   BRIGHT -> FIRST-TRANSACTION GLITCH THEORY CONFIRMED.
 *             Fix for solar_os: dummy tx while CS=1 before selecting panel.
 *   BLACK  -> theory wrong; remaining deltas: 20ms delay / psram flag.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"

static const char *TAG = "exp5";

#define LCD_SPI_HOST    SPI3_HOST
#define PIN_MOSI        GPIO_NUM_40
#define PIN_SCLK        GPIO_NUM_41
#define PIN_DC          GPIO_NUM_39
#define PIN_BL          GPIO_NUM_42
#define I2C_SDA         GPIO_NUM_1
#define I2C_SCL         GPIO_NUM_2
#define PCA9557_ADDR    0x19
#define PCLK_HZ         (80 * 1000 * 1000)

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t pca9557_dev;
static esp_lcd_panel_io_handle_t io_handle;
static esp_lcd_panel_handle_t panel;

static esp_err_t pca9557_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(pca9557_dev, buf, sizeof(buf), 100);
}

static void fill(uint16_t color)
{
    uint16_t *line = heap_caps_malloc(320 * sizeof(uint16_t),
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (line == NULL) return;
    for (int i = 0; i < 320; i++) line[i] = color;
    for (int y = 0; y < 240; y++) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, 320, y + 1, line);
    }
    heap_caps_free(line);
}

void app_main(void)
{
    /* 1. I2C + PCA9557 CS=1 guard (identical to exp2/exp4) */
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = I2C_SDA;
    bus_cfg.scl_io_num = I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = PCA9557_ADDR;
    dev_cfg.scl_speed_hz = 100000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &dev_cfg, &pca9557_dev));

    ESP_ERROR_CHECK(pca9557_write(0x01, 0x03));
    ESP_ERROR_CHECK(pca9557_write(0x03, 0xf8));
    ESP_LOGI(TAG, "guard: CS=1 before SPI init");

    /* 2. SPI bus init (identical to exp2/exp4) */
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = PIN_SCLK;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = 320 * 240 * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* 3. panel io + panel (identical to exp2/exp4: NO psram_dma_direct) */
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = GPIO_NUM_NC;
    io_cfg.dc_gpio_num = PIN_DC;
    io_cfg.spi_mode = 2;
    io_cfg.pclk_hz = PCLK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));

    /* *** THE ONLY CHANGE vs EXP4: SACRIFICIAL DUMMY TRANSACTION while CS=1 ***
     * esp_lcd_panel_reset with no RST pin = tx_param(SWRESET) + 20ms.
     * Panel ignores it (CS high) but the FIRST bus transaction's glitch
     * is absorbed here. Exactly like golden reference. */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_LOGI(TAG, "sacrificial dummy SWRESET issued at CS=1");

    /* 4. CS=0 select (identical) */
    ESP_ERROR_CHECK(pca9557_write(0x01, 0x02));
    /* 5. minimal init (now SECOND transaction - clean) */
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_LOGI(TAG, "init done (dummy-first sequence)");

    /* Backlight (identical) */
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_10_BIT;
    timer_cfg.timer_num = LEDC_TIMER_1;
    timer_cfg.freq_hz = 5000;
    timer_cfg.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num = PIN_BL;
    ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg.channel = LEDC_CHANNEL_0;
    ch_cfg.timer_sel = LEDC_TIMER_1;
    ch_cfg.duty = 512;
    ch_cfg.hpoint = 0;
    ch_cfg.flags.output_invert = true;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    /* 6. color cycle 15s (identical) */
    const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
    for (int cycle = 0; cycle < 15; cycle++) {
        fill(colors[cycle % 4]);
        ESP_LOGI(TAG, "cycle %d color 0x%04x", cycle, colors[cycle % 4]);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_ERROR_CHECK(pca9557_write(0x01, 0x03));
    ESP_LOGI(TAG, "done, CS=1 restored");
}
