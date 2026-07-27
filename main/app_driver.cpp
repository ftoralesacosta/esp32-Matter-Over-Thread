/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_log.h>
#include <stdlib.h>
#include <string.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <app_priv.h>
#include <common_macros.h>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_button.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t fan_endpoint_id;

#if CONFIG_IDF_TARGET_ESP32H2
// Waveshare ESP32-H2-Zero: GPIO12 is a plain, unshared GPIO on this board.
// Deliberately NOT using GPIO13/14 (32.768kHz crystal per Espressif's
// official DevKitM-1 guide) or GPIO23/24 (UART0 TX/RX - the serial console
// this project's whole flashing workflow depends on).
#define PWM_FAN_GPIO            GPIO_NUM_12
#else
#define PWM_FAN_GPIO            GPIO_NUM_21       // Physical pin D3 on Seeed Studio XIAO ESP32-C6
#endif
#define PWM_LEDC_CHANNEL        LEDC_CHANNEL_0
#define PWM_LEDC_TIMER          LEDC_TIMER_0
#define PWM_LEDC_MODE           LEDC_LOW_SPEED_MODE
#define PWM_LEDC_RESOLUTION     LEDC_TIMER_10_BIT // 10-bit resolution (0 to 1023)
#define PWM_LEDC_FREQ           25000             // 25 kHz PWM frequency for Noctua fan

static uint8_t current_speed_percentage = 0;


static esp_err_t app_driver_fan_set_speed(uint8_t speed_percentage)
{
    if (speed_percentage > 100) {
        speed_percentage = 100;
    }

    // Map speed percentage (0-100) to LEDC duty cycle (0-1023)
    uint32_t duty = (speed_percentage * 1023) / 100;

    esp_err_t err = ledc_set_duty(PWM_LEDC_MODE, PWM_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LEDC duty: %s", esp_err_to_name(err));
        return err;
    }

    err = ledc_update_duty(PWM_LEDC_MODE, PWM_LEDC_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update LEDC duty: %s", esp_err_to_name(err));
        return err;
    }

    current_speed_percentage = speed_percentage;
    ESP_LOGI(TAG, "Fan speed updated to %d%% (Duty: %ld)", speed_percentage, duty);
    return ESP_OK;
}

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    uint16_t endpoint_id = fan_endpoint_id;
    uint32_t cluster_id = FanControl::Id;
    uint32_t attribute_id = FanControl::Attributes::PercentSetting::Id;

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);
    if (!attribute) {
        ESP_LOGE(TAG, "Failed to get PercentSetting attribute");
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();
        return;
    }

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute::get_val(attribute, &val);

    // Toggle: if running, turn off; if off, turn on at DEFAULT_FAN_SPEED
    if (val.val.u8 > 0) {
        val.val.u8 = 0;
    } else {
        val.val.u8 = DEFAULT_FAN_SPEED;
    }

    attribute::update(endpoint_id, cluster_id, attribute_id, &val);

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

static esp_timer_handle_t debounce_timer = NULL;
static uint8_t target_speed = 0;
static uint16_t target_endpoint_id = 0;
static bool timer_created = false;
// Set while the debounce callback pushes PercentCurrent/FanMode back into the
// Matter data model. attribute::update() re-enters the attribute-update
// callback path; without this guard a driver-initiated write would reschedule
// the very timer whose callback is running, re-arming it forever. This is why
// the debounce "never fired" in practice.
static bool driver_initiated_update = false;

static void debounce_timer_callback(void *arg)
{
    // 1. Update physical fan speed
    app_driver_fan_set_speed(target_speed);

    // 2. Update database attributes (PercentCurrent and FanMode)
    uint16_t endpoint_id = target_endpoint_id;

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    // Mark these writes as driver-initiated so the attribute-update callback
    // does not treat them as a fresh user request and reschedule the timer.
    driver_initiated_update = true;

    // Update PercentCurrent to match target_speed
    esp_matter_attr_val_t current_val = esp_matter_uint8(target_speed);
    esp_err_t err = attribute::update(endpoint_id, FanControl::Id, FanControl::Attributes::PercentCurrent::Id, &current_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update PercentCurrent in debounce: %s", esp_err_to_name(err));
    }
    
    // Update FanMode to match target_speed (3/High for On, 0 for Off).
    // CHIP's FanControlCluster PreAttributeChangedCallback treats a raw write of
    // FanModeEnum::kOn (4) as a convenience value: it auto-substitutes the concrete
    // FanModeEnum::kHigh (3) and reports back Status::WriteIgnored (240) for the
    // original write, which esp-matter's WriteAttribute wrapper logs as a hard
    // ESP_FAIL even though the substituted value is applied correctly. Writing the
    // concrete kHigh value directly here produces the identical stored state
    // without going through that substitution path, so the ESP_FAIL log next to
    // every debounce firing was misleading, not a real failure.
    uint8_t mode = (target_speed > 0) ? 3 : 0;
    esp_matter_attr_val_t mode_val = esp_matter_enum8(mode);
    err = attribute::update(endpoint_id, FanControl::Id, FanControl::Attributes::FanMode::Id, &mode_val);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update FanMode in debounce: %s", esp_err_to_name(err));
    }

    driver_initiated_update = false;

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    ESP_LOGI(TAG, "Debounce timer fired: Fan speed applied and database synchronized to %d%%", target_speed);
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == fan_endpoint_id) {
        if (cluster_id == FanControl::Id) {
            // Ignore writes that this driver itself issued from the debounce
            // callback (PercentCurrent/FanMode sync). Only genuine user-driven
            // PercentSetting writes should (re)arm the debounce timer.
            if (driver_initiated_update) {
                return ESP_OK;
            }
            if (attribute_id == FanControl::Attributes::PercentSetting::Id) {
                uint8_t speed = val->val.u8;
                
                // Store the target speed and endpoint
                target_speed = speed;
                target_endpoint_id = endpoint_id;

                // Create the timer if it doesn't exist yet
                if (!timer_created) {
                    esp_timer_create_args_t timer_args = {
                        .callback = debounce_timer_callback,
                        .arg = NULL,
                        .dispatch_method = ESP_TIMER_TASK,
                        .name = "fan_debounce_timer"
                    };
                    esp_err_t timer_err = esp_timer_create(&timer_args, &debounce_timer);
                    if (timer_err == ESP_OK) {
                        timer_created = true;
                    } else {
                        ESP_LOGE(TAG, "Failed to create debounce timer: %s", esp_err_to_name(timer_err));
                        // Fallback to immediate update if timer creation fails
                        return app_driver_fan_set_speed(speed);
                    }
                }

                // Stop the timer if it's already running (reset the debounce window)
                esp_timer_stop(debounce_timer);

                // Start the timer with a 300ms delay (300,000 microseconds)
                esp_timer_start_once(debounce_timer, 300000);
                
                ESP_LOGD(TAG, "Debounce timer scheduled for speed %d%%", speed);
            }
        }
    }
    return err;
}

esp_err_t app_driver_attribute_post_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                           uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    // Database updates for PercentCurrent and FanMode are now handled in the debounce timer callback
    return ESP_OK;
}

esp_err_t app_driver_fan_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = ESP_OK;
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
 
    attribute_t *attribute = attribute::get(endpoint_id, FanControl::Id, FanControl::Attributes::PercentSetting::Id);
    if (attribute) {
        attribute::get_val(attribute, &val);
        err = app_driver_fan_set_speed(val.val.u8);
    }
    return err;
}



app_driver_handle_t app_driver_fan_init()
{
    // Release the pin from JTAG/strapping and route it to the GPIO matrix
    gpio_reset_pin(PWM_FAN_GPIO);
    gpio_set_direction(PWM_FAN_GPIO, GPIO_MODE_OUTPUT);

    // 1. Configure LEDC Timer
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = PWM_LEDC_MODE,
        .duty_resolution  = PWM_LEDC_RESOLUTION,
        .timer_num        = PWM_LEDC_TIMER,
        .freq_hz          = PWM_LEDC_FREQ,
        .clk_cfg          = LEDC_AUTO_CLK  // Automatically select best clock source
    };
    
    esp_err_t err = ledc_timer_config(&ledc_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return NULL;
    }

    // 2. Configure LEDC Channel
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = PWM_FAN_GPIO,
        .speed_mode     = PWM_LEDC_MODE,
        .channel        = PWM_LEDC_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = PWM_LEDC_TIMER,
        .duty           = 0,
        .hpoint         = 0
    };

    err = ledc_channel_config(&ledc_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return NULL;
    }

    ESP_LOGI(TAG, "Noctua Fan LEDC PWM initialized at 25kHz on GPIO %d", PWM_FAN_GPIO);


    return (app_driver_handle_t)1; // Return non-null handle to indicate success
}

app_driver_handle_t app_driver_button_init()
{
    button_config_t config;
    memset(&config, 0, sizeof(button_config_t));

    config.type = BUTTON_TYPE_GPIO;
#if CONFIG_IDF_TARGET_ESP32H2
    // Waveshare ESP32-H2-Zero: external button on a clean, unshared GPIO.
    // Deliberately not reusing the onboard BOOT button (GPIO9) - holding it
    // low at power-on enters the bootloader.
    config.gpio_button_config.gpio_num = GPIO_NUM_10;
#else
    config.gpio_button_config.gpio_num = GPIO_NUM_2; // Changed back to GPIO 2 for consistency with original repo
#endif
    config.gpio_button_config.active_level = 1;      // Active high (1)

    button_handle_t handle = iot_button_create(&config);
    if (!handle) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    esp_err_t err = iot_button_register_cb(handle, BUTTON_PRESS_DOWN, app_driver_button_toggle_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register button callback");
        return NULL;
    }

    return (app_driver_handle_t)handle;
}
