/* GPIO0 (BOOT key) test — poll + interrupt, print level transitions.
 *
 * Hardware: 立创·实战派 ESP32-S3
 *   BOOT key → GPIO0, active low (pressed = 0, released = 1)
 *   Internal pull-up enabled.
 *
 * What it does:
 *   1. Configure GPIO0 as input + pull-up + any-edge interrupt.
 *   2. Poll every 10 ms and print on level change (debounce 30 ms).
 *   3. ISR prints edge type (rising=release / falling=press).
 *   4. Print a counter every second so we know the program is alive.
 *
 * Expected: press BOOT → "FALLING edge (pressed), level=0"
 *           release  → "RISING edge (released), level=1"
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "keytest";

#define KEY_GPIO        GPIO_NUM_0
#define DEBOUNCE_MS    30

static QueueHandle_t isr_queue = NULL;

static void IRAM_ATTR key_isr(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(isr_queue, &gpio_num, NULL);
}

static void isr_task(void *arg)
{
    uint32_t gpio_num;
    for (;;) {
        if (xQueueReceive(isr_queue, &gpio_num, portMAX_DELAY)) {
            int level = gpio_get_level(gpio_num);
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));   /* debounce */
            int level2 = gpio_get_level(gpio_num);
            if (level != level2) continue;             /* bounce, ignore */
            if (level == 0)
                printf("[ISR] FALLING edge (pressed),  level=%d\n", level);
            else
                printf("[ISR] RISING  edge (released), level=%d\n", level);
        }
    }
}

static void poll_task(void *arg)
{
    int last = gpio_get_level(KEY_GPIO);
    printf("[POLL] initial level=%d (1=released, 0=pressed)\n", last);
    for (;;) {
        int now = gpio_get_level(KEY_GPIO);
        if (now != last) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            int chk = gpio_get_level(KEY_GPIO);
            if (chk != now) { last = chk; continue; }  /* bounce */
            if (now == 0)
                printf("[POLL] *** PRESSED  *** level=%d\n", now);
            else
                printf("[POLL] *** RELEASED *** level=%d\n", now);
            last = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void alive_task(void *arg)
{
    int sec = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("[ALIVE] %d s, level=%d\n", ++sec, gpio_get_level(KEY_GPIO));
    }
}

void app_main(void)
{
    printf("=== GPIO0 BOOT Key Test ===\n");
    printf("Key: GPIO0, active low, internal pull-up\n");
    printf("Press the BOOT button...\n\n");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        printf("gpio_config failed: %s\n", esp_err_to_name(err));
        return;
    }

    isr_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(isr_task, "isr_task", 4096, NULL, 10, NULL);
    xTaskCreate(poll_task, "poll_task", 4096, NULL, 9, NULL);
    xTaskCreate(alive_task, "alive_task", 4096, NULL, 1, NULL);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(KEY_GPIO, key_isr, (void *)KEY_GPIO);

    printf("Setup done. Waiting for key presses...\n");
}
