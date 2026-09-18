#include "solar_os_wave35b_core.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_log.h"

/*
 * Waveshare ESP32-S3-Touch-LCD-3.5B power/reset sequencing, following the
 * xiaozhi reference firmware port that shipped in MicroPixel (proven to
 * light the panel):
 *
 *   1. TCA9554 (0x20): clear polarity, set P0+P1 as outputs, hold both low
 *      100 ms, then release P1 high (panel reset line) while P0 stays low.
 *   2. AXP2101 (0x34): power-button tuning, DC1-only converter policy,
 *      rail voltages (DC1 3.3 V, ALDO1 3.3 V, BLDO1 1.5 V, BLDO2 2.8 V),
 *      enable ALDO1/BLDO1/BLDO2, charger tuning (4.1 V CV).
 *
 * Everything else stays off; the display driver runs after this device
 * attaches (it is `early` and listed first among the board's fixed devices).
 */

#define TCA9554_REG_OUTPUT_PORT 0x01U
#define TCA9554_REG_POLARITY_PORT 0x02U
#define TCA9554_REG_CONFIGURATION_PORT 0x03U

#define TCA9554_PIN0 (1U << 0U)
#define TCA9554_PIN1 (1U << 1U)
#define TCA9554_EXPANDER_STAGE_DELAY_MS 100U

#define AXP2101_REG_POWER_OFF_SOURCE 0x22U
#define AXP2101_REG_POWER_OFF_BUTTON 0x27U
#define AXP2101_REG_DC_ENABLE 0x80U
#define AXP2101_REG_LDO_ENABLE_LOW 0x90U
#define AXP2101_REG_LDO_ENABLE_HIGH 0x91U
#define AXP2101_REG_DC1_VOLTAGE 0x82U
#define AXP2101_REG_ALDO1_VOLTAGE 0x92U
#define AXP2101_REG_BLDO1_VOLTAGE 0x96U
#define AXP2101_REG_BLDO2_VOLTAGE 0x97U
#define AXP2101_REG_CHARGER_VOLTAGE 0x64U
#define AXP2101_REG_PRECHARGE_CURRENT 0x61U
#define AXP2101_REG_CHARGE_CURRENT 0x62U
#define AXP2101_REG_TERM_CURRENT 0x63U

#define AXP2101_DC_ENABLE_DC1_ONLY 0x01U
#define AXP2101_LDO_ENABLE_ALDO1_BLDO1_BLDO2 0x31U
#define AXP2101_POWER_OFF_SOURCE_VALUE 0x06U
#define AXP2101_POWER_OFF_BUTTON_VALUE 0x10U

#define AXP2101_DC1_MV 3300
#define AXP2101_ALDO1_MV 3300
#define AXP2101_BLDO1_MV 1500
#define AXP2101_BLDO2_MV 2800

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_BUS_NAME_MAX];
    uint8_t pmic_address;
    uint8_t expander_address;
} solar_os_wave35b_core_device_t;

static const char *TAG = "wave35b-core";
static solar_os_wave35b_core_device_t core_device;

static esp_err_t write_reg(const char *bus, uint8_t address, uint8_t reg,
                           uint8_t value)
{
    return solar_os_bus_i2c_write_reg(bus, address, reg, &value, 1);
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *pmic_address,
                                uint8_t *expander_address)
{
    bool have_i2c = false;
    bool have_pmic = false;
    bool have_expander = false;

    if (bindings == NULL || i2c_bus == NULL || pmic_address == NULL ||
        expander_address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *pmic_address = 0U;
    *expander_address = 0U;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (binding->value == SOLAR_OS_WAVE35B_CORE_PMIC_ADDRESS &&
                !have_pmic) {
                *pmic_address = (uint8_t)binding->value;
                have_pmic = true;
            } else if (binding->value == SOLAR_OS_WAVE35B_CORE_EXPANDER_ADDRESS &&
                       !have_expander) {
                *expander_address = (uint8_t)binding->value;
                have_expander = true;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_pmic && have_expander
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t run_reset_pulse(const char *bus, uint8_t expander_address)
{
    ESP_RETURN_ON_ERROR(
        write_reg(bus, expander_address, TCA9554_REG_POLARITY_PORT, 0x00U),
        TAG, "clear TCA9554 polarity failed");
    /* P0+P1 outputs, all other pins stay inputs. */
    ESP_RETURN_ON_ERROR(
        write_reg(bus, expander_address, TCA9554_REG_CONFIGURATION_PORT,
                  (uint8_t)(0xFFU & ~(TCA9554_PIN0 | TCA9554_PIN1))),
        TAG, "configure TCA9554 directions failed");
    vTaskDelay(pdMS_TO_TICKS(TCA9554_EXPANDER_STAGE_DELAY_MS));
    /* Hold both control lines low. */
    ESP_RETURN_ON_ERROR(
        write_reg(bus, expander_address, TCA9554_REG_OUTPUT_PORT,
                  (uint8_t)(~(TCA9554_PIN0 | TCA9554_PIN1) & 0xFFU)),
        TAG, "hold panel reset failed");
    vTaskDelay(pdMS_TO_TICKS(TCA9554_EXPANDER_STAGE_DELAY_MS));
    /* Release the panel reset (P1 high, P0 stays low). */
    ESP_RETURN_ON_ERROR(
        write_reg(bus, expander_address, TCA9554_REG_OUTPUT_PORT,
                  TCA9554_PIN1),
        TAG, "release panel reset failed");
    return ESP_OK;
}

static esp_err_t run_pmic_setup(const char *bus, uint8_t pmic_address)
{
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_POWER_OFF_SOURCE,
                  AXP2101_POWER_OFF_SOURCE_VALUE),
        TAG, "configure power-off source failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_POWER_OFF_BUTTON,
                  AXP2101_POWER_OFF_BUTTON_VALUE),
        TAG, "configure power-off button failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_DC_ENABLE,
                  AXP2101_DC_ENABLE_DC1_ONLY),
        TAG, "select DC1-only policy failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_LDO_ENABLE_LOW, 0x00U),
        TAG, "disable low LDO bank failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_LDO_ENABLE_HIGH, 0x00U),
        TAG, "disable high LDO bank failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_DC1_VOLTAGE,
                  (uint8_t)((AXP2101_DC1_MV - 1500) / 100)),
        TAG, "set DC1 voltage failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_ALDO1_VOLTAGE,
                  (uint8_t)((AXP2101_ALDO1_MV - 500) / 100)),
        TAG, "set ALDO1 voltage failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_BLDO1_VOLTAGE,
                  (uint8_t)((AXP2101_BLDO1_MV - 500) / 100)),
        TAG, "set BLDO1 voltage failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_BLDO2_VOLTAGE,
                  (uint8_t)((AXP2101_BLDO2_MV - 500) / 100)),
        TAG, "set BLDO2 voltage failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_LDO_ENABLE_LOW,
                  AXP2101_LDO_ENABLE_ALDO1_BLDO1_BLDO2),
        TAG, "enable ALDO1/BLDO1/BLDO2 failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_CHARGER_VOLTAGE, 0x02U),
        TAG, "set charger CV failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_PRECHARGE_CURRENT, 0x02U),
        TAG, "set precharge current failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_CHARGE_CURRENT, 0x04U),
        TAG, "set charge current failed");
    ESP_RETURN_ON_ERROR(
        write_reg(bus, pmic_address, AXP2101_REG_TERM_CURRENT, 0x01U),
        TAG, "set termination current failed");
    return ESP_OK;
}

esp_err_t solar_os_wave35b_core_attach(const char *name,
                                       const solar_os_expansion_binding_t *bindings,
                                       size_t binding_count)
{
    char i2c_bus[SOLAR_OS_BUS_NAME_MAX] = {0};
    uint8_t pmic_address = 0U;
    uint8_t expander_address = 0U;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (core_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(
        parse_bindings(bindings, binding_count, i2c_bus, sizeof(i2c_bus),
                       &pmic_address, &expander_address),
        TAG, "invalid bindings");
    const esp_err_t expander_probe =
        solar_os_bus_i2c_probe(i2c_bus, expander_address);
    if (expander_probe != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "TCA9554 0x%02x probe failed: %s",
                      (unsigned)expander_address,
                      esp_err_to_name(expander_probe));
        return expander_probe;
    }
    const esp_err_t pmic_probe = solar_os_bus_i2c_probe(i2c_bus, pmic_address);
    if (pmic_probe != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "AXP2101 0x%02x probe failed: %s",
                      (unsigned)pmic_address, esp_err_to_name(pmic_probe));
        return pmic_probe;
    }

    ESP_RETURN_ON_ERROR(run_reset_pulse(i2c_bus, expander_address), TAG,
                        "TCA9554 reset pulse failed");
    ESP_RETURN_ON_ERROR(run_pmic_setup(i2c_bus, pmic_address), TAG,
                        "AXP2101 setup failed");

    memset(&core_device, 0, sizeof(core_device));
    core_device.active = true;
    core_device.pmic_address = pmic_address;
    core_device.expander_address = expander_address;
    strlcpy(core_device.name, name, sizeof(core_device.name));
    strlcpy(core_device.i2c_bus, i2c_bus, sizeof(core_device.i2c_bus));

    ESP_LOGI(TAG, "%s attached on %s: panel reset released, PMIC rails set",
             name, i2c_bus);
    SOLAR_OS_LOGI(TAG, "%s attached: panel reset released, PMIC rails set",
                  name);
    return ESP_OK;
}

esp_err_t solar_os_wave35b_core_detach(const char *name)
{
    if (!core_device.active || name == NULL ||
        strcmp(core_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Board-origin device: leave the rails and panel reset as-is on detach
     * so the display and touch devices behind them stay functional. */
    memset(&core_device, 0, sizeof(core_device));
    return ESP_OK;
}
