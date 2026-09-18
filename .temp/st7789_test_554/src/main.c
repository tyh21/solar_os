/* DECISIVE EXPERIMENT: reproduce solar_os init order in the test program.
 *
 * Phase 1 (RED): exactly solar_os order — spi_bus_initialize FIRST while
 *   PCA9557 CS is floating (panel absorbs garbage), then configure
 *   PCA9557 (CS=1), settle, select CS=0, SWRESET, minimal init, fill RED.
 *   -> If screen stays BLACK, we reproduced the solar_os bug in the
 *      test program = init-order root cause CONFIRMED.
 *
 * Phase 2 (GREEN, rescue): regardless of phase 1 outcome, re-do a full
 *   register-level re-init (SWRESET -> SLPOUT -> full register set ->
 *   DISPON), fill GREEN.  -> If GREEN appears, the rescue sequence works
 *      and can be ported to solar_os.
 *
 * Phase 3 (BLUE): normal draw via the same panel handle, sanity check.
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

static const char *TAG = "st7789_exp";

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

static esp_err_t pca9557_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(pca9557_dev, buf, sizeof(buf), 100);
}

/* Send one command with params via tx_param. */
static esp_err_t st_cmd(esp_lcd_panel_io_handle_t io, uint8_t cmd,
                        const uint8_t *params, size_t n)
{
    return esp_lcd_panel_io_tx_param(io, cmd, params, n);
}

static void fill(esp_lcd_panel_handle_t panel, uint16_t color)
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
    /* --- I2C bus (needed for PCA9557 writes later) --- */
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

    /* ================= PHASE 1: solar_os order (RED) ================= */
    /* SPI bus init FIRST — PCA9557 not yet configured, CS floating. */
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = PIN_SCLK;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = 320 * 240 * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "PHASE1: SPI bus initialized FIRST (CS floating) — solar_os order");

    /* Now configure PCA9557 (same as solar_os attach_tft does): CS=1. */
    ESP_ERROR_CHECK(pca9557_write(0x01, 0x07));   /* output: CS=1, PA_EN=1, PWDN=0 */
    ESP_ERROR_CHECK(pca9557_write(0x03, 0xf8));   /* config: P0/P1/P2 outputs */
    ESP_LOGI(TAG, "PHASE1: PCA9557 configured CS=1 (after SPI init)");

    /* Settle (solar_os does 200ms). */
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = GPIO_NUM_NC;
    io_cfg.dc_gpio_num = PIN_DC;
    io_cfg.spi_mode = 2;
    io_cfg.pclk_hz = PCLK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    /* solar_os current attempt: CS low BEFORE reset, SWRESET reaches panel. */
    ESP_ERROR_CHECK(pca9557_write(0x01, 0x06));   /* CS=0 */
    ESP_ERROR_CHECK(st_cmd(io, 0x01, NULL, 0));   /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_LOGI(TAG, "PHASE1: SWRESET sent with CS=0, 120ms");

    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_LOGI(TAG, "PHASE1: minimal init done — expect RED");

    /* Backlight (identical to solar_os: GPIO42 low-active PWM 5kHz). */
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
    ESP_LOGI(TAG, "Backlight ON");

    fill(panel, 0xF800);  /* RED */
    ESP_LOGI(TAG, "PHASE1: RED filled (t=0s)");

    vTaskDelay(pdMS_TO_TICKS(5000));   /* 5s RED observation window */

    /* ============ PHASE 2: rescue full re-init (GREEN) ============ */
    /* Full ST7789 register sequence — rescue a panel that ate garbage. */
    ESP_ERROR_CHECK(st_cmd(io, 0x01, NULL, 0));          /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* SLPOUT */
    ESP_ERROR_CHECK(st_cmd(io, 0x11, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(120));

    const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    ESP_ERROR_CHECK(st_cmd(io, 0xB2, porctrl, sizeof(porctrl)));   /* PORCTRL */
    const uint8_t gctrl[] = {0x45};
    ESP_ERROR_CHECK(st_cmd(io, 0xB7, gctrl, sizeof(gctrl)));       /* GCTRL */
    const uint8_t vrhs[] = {0x11};
    ESP_ERROR_CHECK(st_cmd(io, 0xBB, vrhs, sizeof(vrhs)));         /* VRHS */
    const uint8_t vcoms[] = {0x2C};
    ESP_ERROR_CHECK(st_cmd(io, 0xC0, vcoms, sizeof(vcoms)));       /* VCOMS */
    const uint8_t frctrl2[] = {0x00};
    ESP_ERROR_CHECK(st_cmd(io, 0xC6, frctrl2, sizeof(frctrl2)));    /* FRCTRL2 */
    const uint8_t pwctrl1[] = {0xA4, 0xA1};
    ESP_ERROR_CHECK(st_cmd(io, 0xD0, pwctrl1, sizeof(pwctrl1)));    /* PWCTRL1 */

    /* Gamma (ST7789V standard values from tft_ili9341.c). */
    static const uint8_t pgamma[] = {
        0xD0, 0x00, 0x05, 0x0E, 0x15, 0x0D, 0x37, 0x43,
        0x47, 0x09, 0x15, 0x12, 0x16, 0x19
    };
    ESP_ERROR_CHECK(st_cmd(io, 0xE0, pgamma, sizeof(pgamma)));      /* PVGAMMA */
    static const uint8_t ngamma[] = {
        0xD0, 0x0B, 0x0E, 0x10, 0x10, 0x0C, 0x3B, 0x44,
        0x06, 0x17, 0x0D, 0x0F, 0x0E, 0x12
    };
    ESP_ERROR_CHECK(st_cmd(io, 0xE1, ngamma, sizeof(ngamma)));       /* NVGAMMA */

    ESP_ERROR_CHECK(st_cmd(io, 0x21, NULL, 0));   /* INVON */
    ESP_ERROR_CHECK(st_cmd(io, 0x36, (const uint8_t[]){0x60}, 1));   /* MADCTL */
    ESP_ERROR_CHECK(st_cmd(io, 0x3A, (const uint8_t[]){0x55}, 1));   /* COLMOD 16bpp */
    ESP_ERROR_CHECK(st_cmd(io, 0xB0, (const uint8_t[]){0x00}, 1));   /* RAMCTRL */
    ESP_ERROR_CHECK(st_cmd(io, 0x29, NULL, 0));   /* DISPON */
    ESP_LOGI(TAG, "PHASE2: rescue full re-init done");

    fill(panel, 0x07E0);  /* GREEN */
    ESP_LOGI(TAG, "PHASE2: GREEN filled (t=5s)");

    vTaskDelay(pdMS_TO_TICKS(5000));   /* 5s GREEN window */

    /* ================= PHASE 3: sanity (BLUE) ================= */
    fill(panel, 0x001F);
    ESP_LOGI(TAG, "PHASE3: BLUE filled (t=10s). If you saw RED/GREEN/BLUE all good.");
}
