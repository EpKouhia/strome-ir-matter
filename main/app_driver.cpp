/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <button_gpio.h>
#include <ds18b20.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <iot_button.h>
#include <math.h>
#include <platform/PlatformManager.h>

#include <app_priv.h>
#include <trotec_3550_ir.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t room_air_conditioner_endpoint_id;
extern uint16_t room_temperature_sensor_endpoint_id;
static constexpr gpio_num_t kButtonGpio = GPIO_NUM_9;
static constexpr gpio_num_t kDs18b20Gpio = (gpio_num_t)DS18B20_GPIO;
static constexpr int16_t kFallbackLocalTemperature = 2000; // 20.0 C until the external sensor has a valid reading.
static constexpr int16_t kMinReportedRoomTemperature = 500; // 5.0 C, avoids reporting missing-sensor 0 C.
static constexpr int16_t kMaxReportedRoomTemperature = 4500; // 45.0 C is enough for indoor AC control.
static constexpr uint16_t kThermostatRunningStateCool = 0x0002;
static constexpr uint8_t kFanModeOff = 0;
static constexpr uint8_t kFanModeLow = 1;
static constexpr uint8_t kFanModeMedium = 2;
static constexpr uint8_t kFanModeHigh = 3;
static constexpr uint8_t kFanPercentLow = 33;
static constexpr uint8_t kFanPercentMedium = 66;
static constexpr uint8_t kFanPercentHigh = 100;
static constexpr uint8_t kFanSpeedMax = 3;
static constexpr uint8_t kSupportedRockSettingMask = FAN_SWING_UP_DOWN;
static constexpr uint8_t kThermostatModeOff = 0;
static constexpr uint8_t kThermostatModeCool = 3;
static constexpr uint8_t kThermostatModeHeat = 4;

// AC device state
static ac_device_state_t ac_state = {
    .power_on = DEFAULT_POWER,
    .mode = (ac_mode_t)DEFAULT_AC_MODE,
    .fan_speed = (fan_speed_t)DEFAULT_FAN_SPEED,
    .fan_swing = DEFAULT_FAN_SWING,
    .temperature = DEFAULT_TEMPERATURE
};

static ac_mode_t s_last_active_mode = (ac_mode_t)DEFAULT_AC_MODE;
static bool s_syncing_power_mode_attributes = false;
static bool s_syncing_fan_attributes = false;
static uint16_t s_last_pre_update_endpoint_id = 0;
static uint32_t s_last_pre_update_cluster_id = 0;
static uint32_t s_last_pre_update_attribute_id = 0;
static bool s_last_pre_update_sent_ir = false;
static int16_t s_reported_local_temperature = kFallbackLocalTemperature;
static TaskHandle_t s_temperature_sensor_task = nullptr;

void app_driver_note_pre_update_result(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_err_t err)
{
    s_last_pre_update_endpoint_id = endpoint_id;
    s_last_pre_update_cluster_id = cluster_id;
    s_last_pre_update_attribute_id = attribute_id;
    s_last_pre_update_sent_ir = err == ESP_OK;
}

bool app_driver_was_pre_update_handled(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id)
{
    return s_last_pre_update_sent_ir && s_last_pre_update_endpoint_id == endpoint_id &&
           s_last_pre_update_cluster_id == cluster_id && s_last_pre_update_attribute_id == attribute_id;
}

static bool is_supported_thermostat_mode(uint8_t mode)
{
    return mode == kThermostatModeOff || mode == kThermostatModeCool || mode == kThermostatModeHeat ||
           mode == AC_MODE_FAN || mode == AC_MODE_DEHUMIDIFY;
}

static bool is_supported_temperature(int16_t temperature)
{
    return temperature >= MIN_TEMPERATURE && temperature <= MAX_TEMPERATURE;
}

static bool is_supported_fan_mode(uint8_t fan_mode)
{
    return fan_mode == kFanModeLow || fan_mode == kFanModeMedium || fan_mode == kFanModeHigh;
}

static bool attr_val_is_null(const esp_matter_attr_val_t *val)
{
    if (!val) {
        return true;
    }

    switch (val->type) {
        case ESP_MATTER_VAL_TYPE_NULLABLE_UINT8:
        case ESP_MATTER_VAL_TYPE_NULLABLE_ENUM8:
        case ESP_MATTER_VAL_TYPE_NULLABLE_BITMAP8:
            return val->val.u8 == UINT8_MAX;
        case ESP_MATTER_VAL_TYPE_NULLABLE_INT16:
            return val->val.i16 == INT16_MIN;
        default:
            return false;
    }
}

static uint8_t attr_val_to_uint8(const esp_matter_attr_val_t *val)
{
    return val ? val->val.u8 : 0;
}

static const char *attr_val_uint8_to_log_str(const esp_matter_attr_val_t *val, char *buffer, size_t buffer_len)
{
    if (attr_val_is_null(val)) {
        return "null";
    }
    snprintf(buffer, buffer_len, "%u", static_cast<unsigned>(attr_val_to_uint8(val)));
    return buffer;
}

static int16_t attr_val_to_int16(const esp_matter_attr_val_t *val)
{
    return val ? val->val.i16 : 0;
}

static bool attr_val_to_bool(const esp_matter_attr_val_t *val)
{
    return val ? val->val.b : false;
}

static uint8_t ac_mode_to_thermostat_mode(ac_mode_t mode);
static void update_attribute_if_exists(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                       esp_matter_attr_val_t *val);

static void update_reported_temperature()
{
    nullable<int16_t> temperature(s_reported_local_temperature);
    esp_matter_attr_val_t current_temperature = esp_matter_nullable_int16(temperature);
    attribute::update(room_air_conditioner_endpoint_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id,
                      &current_temperature);
    attribute::update(room_air_conditioner_endpoint_id, TemperatureMeasurement::Id,
                      TemperatureMeasurement::Attributes::MeasuredValue::Id, &current_temperature);
    if (room_temperature_sensor_endpoint_id != 0) {
        attribute::update(room_temperature_sensor_endpoint_id, TemperatureMeasurement::Id,
                          TemperatureMeasurement::Attributes::MeasuredValue::Id, &current_temperature);
    }
}

static bool local_temperature_is_reasonable(int16_t temperature)
{
    return temperature >= kMinReportedRoomTemperature && temperature <= kMaxReportedRoomTemperature;
}

static void app_driver_temperature_sensor_task(void *arg)
{
    (void)arg;
    ds18b20_init(kDs18b20Gpio);
    ESP_LOGI(TAG, "DS18B20 local temperature sensor initialized on GPIO%d", (int)kDs18b20Gpio);

    while (true) {
        float temperature_c = ds18b20_get_temp();
        int16_t temperature = (int16_t)lroundf(temperature_c * 100.0f);

        if (isfinite(temperature_c) && local_temperature_is_reasonable(temperature)) {
            if (temperature != s_reported_local_temperature) {
                s_reported_local_temperature = temperature;
                chip::DeviceLayer::PlatformMgr().LockChipStack();
                update_reported_temperature();
                chip::DeviceLayer::PlatformMgr().UnlockChipStack();
                ESP_LOGI(TAG, "Local DS18B20 temperature: %.2f C", temperature_c);
            }
        } else {
            ESP_LOGW(TAG, "Ignoring invalid DS18B20 reading %.2f C; keeping %.2f C",
                     temperature_c, s_reported_local_temperature / 100.0f);
        }

        vTaskDelay(pdMS_TO_TICKS(DS18B20_UPDATE_INTERVAL_MS));
    }
}

static void app_driver_temperature_sensor_start()
{
    if (s_temperature_sensor_task) {
        return;
    }

    BaseType_t created = xTaskCreate(app_driver_temperature_sensor_task, "ds18b20_temp", 3072, NULL, 5,
                                     &s_temperature_sensor_task);
    if (created != pdPASS) {
        s_temperature_sensor_task = nullptr;
        ESP_LOGE(TAG, "Failed to start DS18B20 temperature task; using fallback %.2f C",
                 s_reported_local_temperature / 100.0f);
    }
}

static void sync_thermostat_operating_state()
{
    uint8_t cooling_demand = 0;
    uint8_t running_mode = AC_MODE_OFF;
    uint16_t running_state = 0;

    if (ac_state.power_on) {
        running_mode = ac_mode_to_thermostat_mode(ac_state.mode);
        cooling_demand = (ac_state.mode == AC_MODE_COOL) ? 50 : 0;
        running_state = (ac_state.mode == AC_MODE_COOL) ? kThermostatRunningStateCool : 0;
    }

    esp_matter_attr_val_t val = esp_matter_uint8(cooling_demand);
    update_attribute_if_exists(room_air_conditioner_endpoint_id, Thermostat::Id,
                               Thermostat::Attributes::PICoolingDemand::Id, &val);

    val = esp_matter_enum8(running_mode);
    update_attribute_if_exists(room_air_conditioner_endpoint_id, Thermostat::Id,
                               Thermostat::Attributes::ThermostatRunningMode::Id, &val);

    val = esp_matter_bitmap16(running_state);
    update_attribute_if_exists(room_air_conditioner_endpoint_id, Thermostat::Id,
                               Thermostat::Attributes::ThermostatRunningState::Id, &val);
}

static void update_attribute_if_exists(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                       esp_matter_attr_val_t *val)
{
    if (attribute::get(endpoint_id, cluster_id, attribute_id)) {
        attribute::update(endpoint_id, cluster_id, attribute_id, val);
    }
}

static void sync_temperature_setpoints(int16_t temperature)
{
    esp_matter_attr_val_t setpoint_val = esp_matter_int16(temperature);

    s_syncing_power_mode_attributes = true;
    attribute::update(room_air_conditioner_endpoint_id, Thermostat::Id,
                      Thermostat::Attributes::OccupiedCoolingSetpoint::Id, &setpoint_val);
    update_attribute_if_exists(room_air_conditioner_endpoint_id, Thermostat::Id,
                               Thermostat::Attributes::OccupiedHeatingSetpoint::Id, &setpoint_val);
    s_syncing_power_mode_attributes = false;
}

static void set_attribute_without_callbacks(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                            esp_matter_attr_val_t *val)
{
    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);
    if (attribute) {
        attribute::set_val(attribute, val, false);
    }
}

static void report_attribute_without_callbacks(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id,
                                               esp_matter_attr_val_t *val)
{
    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);
    if (attribute) {
        attribute::report(endpoint_id, cluster_id, attribute_id, val);
    }
}

static uint8_t ac_mode_to_thermostat_mode(ac_mode_t mode)
{
    switch (mode) {
        case AC_MODE_DEHUMIDIFY:
            return kThermostatModeHeat; // Home Assistant compatibility: Heat represents Dry.
        case AC_MODE_COOL:
            return kThermostatModeCool;
        case AC_MODE_FAN:
        case AC_MODE_OFF:
        default:
            return kThermostatModeOff; // Home Assistant compatibility: Off represents Fan when power is on.
    }
}

static ac_mode_t thermostat_mode_to_ac_mode(uint8_t mode)
{
    switch (mode) {
        case kThermostatModeOff:
            return AC_MODE_FAN;
        case kThermostatModeHeat:
            return AC_MODE_DEHUMIDIFY;
        case kThermostatModeCool:
            return AC_MODE_COOL;
        case AC_MODE_FAN:
            return AC_MODE_FAN;
        case AC_MODE_DEHUMIDIFY:
            return AC_MODE_DEHUMIDIFY;
        default:
            return AC_MODE_OFF;
    }
}

static void sync_power_mode_attributes()
{
    s_syncing_power_mode_attributes = true;

    esp_matter_attr_val_t val = esp_matter_bool(ac_state.power_on);
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id,
                                       &val);

    val = esp_matter_enum8(ac_mode_to_thermostat_mode(ac_state.mode));
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, Thermostat::Id,
                                       Thermostat::Attributes::SystemMode::Id, &val);

    s_syncing_power_mode_attributes = false;
}

static uint8_t fan_speed_to_fan_mode(fan_speed_t speed)
{
    switch (speed) {
        case FAN_SPEED_LOW: return kFanModeLow;
        case FAN_SPEED_HIGH: return kFanModeHigh;
        case FAN_SPEED_MEDIUM:
        default: return kFanModeMedium;
    }
}

static uint8_t fan_speed_to_percent(fan_speed_t speed)
{
    switch (speed) {
        case FAN_SPEED_LOW: return kFanPercentLow;
        case FAN_SPEED_HIGH: return kFanPercentHigh;
        case FAN_SPEED_MEDIUM:
        default: return kFanPercentMedium;
    }
}

static fan_speed_t percent_to_fan_speed(uint8_t percent)
{
    if (percent <= kFanPercentLow) {
        return FAN_SPEED_LOW;
    }
    if (percent <= kFanPercentMedium) {
        return FAN_SPEED_MEDIUM;
    }
    return FAN_SPEED_HIGH;
}

static void sync_fan_control_attributes()
{
    s_syncing_fan_attributes = true;

    uint8_t fan_mode = fan_speed_to_fan_mode(ac_state.fan_speed);
    uint8_t percent = fan_speed_to_percent(ac_state.fan_speed);
    uint8_t speed = ac_state.fan_speed;
    if (!ac_state.power_on) {
        fan_mode = kFanModeOff;
        percent = 0;
        speed = 0;
    }

    esp_matter_attr_val_t val = esp_matter_enum8(fan_mode);
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, FanControl::Id,
                                       FanControl::Attributes::FanMode::Id, &val);

    val = esp_matter_nullable_uint8(nullable<uint8_t>(percent));
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, FanControl::Id,
                                       FanControl::Attributes::PercentSetting::Id, &val);

    val = esp_matter_uint8(percent);
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, FanControl::Id,
                                       FanControl::Attributes::PercentCurrent::Id, &val);

    val = esp_matter_nullable_uint8(nullable<uint8_t>(speed));
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, FanControl::Id,
                                       FanControl::Attributes::SpeedSetting::Id, &val);

    val = esp_matter_uint8(speed);
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, FanControl::Id,
                                       FanControl::Attributes::SpeedCurrent::Id, &val);

    val = esp_matter_bitmap8(ac_state.fan_swing ? kSupportedRockSettingMask : 0);
    report_attribute_without_callbacks(room_air_conditioner_endpoint_id, FanControl::Id,
                                       FanControl::Attributes::RockSetting::Id, &val);

    s_syncing_fan_attributes = false;
}

/* Do any conversions/remapping for the actual value here */
static const char *mode_to_str(ac_mode_t mode)
{
    switch (mode) {
        case AC_MODE_OFF: return "OFF";
        case AC_MODE_COOL: return "COOL";
        case AC_MODE_FAN: return "FAN";
        case AC_MODE_DEHUMIDIFY: return "DEHUMIDIFY";
        default: return "UNKNOWN";
    }
}

static const char *fan_speed_to_str(fan_speed_t speed)
{
    switch (speed) {
        case FAN_SPEED_LOW: return "LOW";
        case FAN_SPEED_MEDIUM: return "MEDIUM";
        case FAN_SPEED_HIGH: return "HIGH";
        default: return "UNKNOWN";
    }
}

static esp_err_t app_driver_room_air_conditioner_apply_mode(ac_mode_t mode, uint8_t requested_mode)
{
    if (mode != AC_MODE_OFF) {
        s_last_active_mode = mode;
    }
    ac_state.mode = mode == AC_MODE_OFF ? AC_MODE_OFF : s_last_active_mode;
    ac_state.power_on = ac_state.mode != AC_MODE_OFF;
    sync_power_mode_attributes();
    sync_fan_control_attributes();
    update_reported_temperature();
    sync_thermostat_operating_state();
    ESP_LOGI(TAG, "Ströme AC Mode: %s (requested=%u)", mode_to_str(ac_state.mode), (unsigned)requested_mode);
    return trotec_3550_ir_send_state(&ac_state);
}

static esp_err_t app_driver_room_air_conditioner_set_thermostat_mode(esp_matter_attr_val_t *val)
{
    uint8_t mode = attr_val_to_uint8(val);
    if (!is_supported_thermostat_mode(mode)) {
        ESP_LOGW(TAG, "Rejected unsupported thermostat mode: %u", (unsigned)mode);
        return ESP_ERR_INVALID_ARG;
    }

    return app_driver_room_air_conditioner_apply_mode(thermostat_mode_to_ac_mode(mode), mode);
}

static esp_err_t app_driver_room_air_conditioner_set_power(esp_matter_attr_val_t *val)
{
    bool power_on = attr_val_to_bool(val);
    ac_state.power_on = power_on;
    if (!power_on) {
        ac_state.mode = AC_MODE_OFF;
    } else if (ac_state.mode == AC_MODE_OFF) {
        ac_state.mode = s_last_active_mode;
    }
    sync_power_mode_attributes();
    sync_fan_control_attributes();
    update_reported_temperature();
    sync_thermostat_operating_state();
    ESP_LOGI(TAG, "Ströme AC Power: %s", ac_state.power_on ? "ON" : "OFF");
    return trotec_3550_ir_send_state(&ac_state);
}

static esp_err_t app_driver_room_air_conditioner_set_temperature(esp_matter_attr_val_t *val)
{
    if (attr_val_is_null(val)) {
        ESP_LOGW(TAG, "Rejected null temperature setpoint");
        return ESP_ERR_INVALID_ARG;
    }

    int16_t temperature = attr_val_to_int16(val);
    if (!is_supported_temperature(temperature)) {
        ESP_LOGW(TAG, "Rejected unsupported setpoint: %.1f°C", temperature / 100.0f);
        return ESP_ERR_INVALID_ARG;
    }

    ac_state.temperature = temperature;
    float temp_celsius = temperature / 100.0f;
    ESP_LOGI(TAG, "Temperature: %.1f°C", temp_celsius);
    sync_temperature_setpoints(ac_state.temperature);
    if (ac_state.power_on) {
        update_reported_temperature();
    }
    sync_thermostat_operating_state();
    ESP_LOGI(TAG, "Sending temperature setpoint with current AC mode %s", mode_to_str(ac_state.mode));
    return trotec_3550_ir_send_state(&ac_state);
}

static esp_err_t app_driver_room_air_conditioner_apply_fan_state(bool fan_on, fan_speed_t speed)
{
    bool new_power_on = fan_on;
    ac_mode_t new_mode = fan_on ? (ac_state.mode == AC_MODE_OFF ? s_last_active_mode : ac_state.mode) : AC_MODE_OFF;
    fan_speed_t new_speed = fan_on ? speed : ac_state.fan_speed;

    if (ac_state.power_on == new_power_on && ac_state.mode == new_mode && ac_state.fan_speed == new_speed) {
        sync_power_mode_attributes();
        sync_fan_control_attributes();
        sync_thermostat_operating_state();
        ESP_LOGI(TAG, "Fan write matches current AC state; no IR frame sent");
        return ESP_OK;
    }

    ac_state.power_on = new_power_on;
    ac_state.mode = new_mode;
    if (ac_state.mode != AC_MODE_OFF) {
        s_last_active_mode = ac_state.mode;
    }
    ac_state.fan_speed = new_speed;
    sync_power_mode_attributes();
    sync_fan_control_attributes();
    update_reported_temperature();
    sync_thermostat_operating_state();
    ESP_LOGI(TAG, "Ströme AC Fan: %s speed=%s (%u)", ac_state.power_on ? "ON" : "OFF",
             fan_speed_to_str(ac_state.fan_speed), static_cast<unsigned>(ac_state.fan_speed));
    return trotec_3550_ir_send_state(&ac_state);
}

static esp_err_t app_driver_room_air_conditioner_set_fan_mode(esp_matter_attr_val_t *val)
{
    uint8_t fan_mode = attr_val_to_uint8(val);
    if (fan_mode == kFanModeOff) {
        return app_driver_room_air_conditioner_apply_fan_state(false, ac_state.fan_speed);
    }
    if (fan_mode == 4) { // FanMode::On maps to High in the CHIP Fan Control server.
        return app_driver_room_air_conditioner_apply_fan_state(true, FAN_SPEED_HIGH);
    }
    if (!is_supported_fan_mode(fan_mode)) {
        ESP_LOGW(TAG, "Rejected unsupported FanMode: %u", static_cast<unsigned>(fan_mode));
        return ESP_ERR_INVALID_ARG;
    }

    return app_driver_room_air_conditioner_apply_fan_state(true, static_cast<fan_speed_t>(fan_mode));
}

static esp_err_t app_driver_room_air_conditioner_set_fan_percent(esp_matter_attr_val_t *val)
{
    if (attr_val_is_null(val)) {
        ESP_LOGW(TAG, "Rejected null Fan PercentSetting");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t percent = attr_val_to_uint8(val);
    if (percent == 0) {
        return app_driver_room_air_conditioner_apply_fan_state(false, ac_state.fan_speed);
    }
    if (percent > 100) {
        ESP_LOGW(TAG, "Rejected unsupported Fan PercentSetting: %u", static_cast<unsigned>(percent));
        return ESP_ERR_INVALID_ARG;
    }

    return app_driver_room_air_conditioner_apply_fan_state(true, percent_to_fan_speed(percent));
}

static esp_err_t app_driver_room_air_conditioner_set_fan_speed(esp_matter_attr_val_t *val)
{
    if (attr_val_is_null(val)) {
        ESP_LOGW(TAG, "Rejected null Fan SpeedSetting");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t speed = attr_val_to_uint8(val);
    if (speed == 0) {
        return app_driver_room_air_conditioner_apply_fan_state(false, ac_state.fan_speed);
    }
    if (speed < FAN_SPEED_LOW || speed > kFanSpeedMax) {
        ESP_LOGW(TAG, "Rejected unsupported Fan SpeedSetting: %u", static_cast<unsigned>(speed));
        return ESP_ERR_INVALID_ARG;
    }

    return app_driver_room_air_conditioner_apply_fan_state(true, static_cast<fan_speed_t>(speed));
}

static esp_err_t app_driver_room_air_conditioner_set_rock_setting(esp_matter_attr_val_t *val)
{
    uint8_t rock_setting = attr_val_to_uint8(val);
    if (rock_setting & ~kSupportedRockSettingMask) {
        ESP_LOGW(TAG, "Rejected unsupported Fan RockSetting: 0x%02x", rock_setting);
        return ESP_ERR_INVALID_ARG;
    }

    bool swing_enabled = (rock_setting & kSupportedRockSettingMask) != 0;
    if (ac_state.fan_swing == swing_enabled) {
        sync_fan_control_attributes();
        ESP_LOGI(TAG, "RockSetting write matches current swing state; no IR frame sent");
        return ESP_OK;
    }

    ac_state.fan_swing = swing_enabled;
    sync_fan_control_attributes();
    ESP_LOGI(TAG, "Ströme AC Swing: %s", ac_state.fan_swing ? "ON" : "OFF");
    return trotec_3550_ir_send_state(&ac_state);
}

static void app_driver_button_toggle_cb(void *arg, void *data)
{
    ESP_LOGI(TAG, "Toggle button pressed");
    uint16_t endpoint_id = room_air_conditioner_endpoint_id;
    uint32_t cluster_id = OnOff::Id;
    uint32_t attribute_id = OnOff::Attributes::OnOff::Id;

    attribute_t *attribute = attribute::get(endpoint_id, cluster_id, attribute_id);
    if (!attribute) {
        ESP_LOGW(TAG, "OnOff attribute not found for button toggle");
        return;
    }

    esp_matter_attr_val_t val = esp_matter_invalid(NULL);
    attribute::get_val(attribute, &val);
    val = esp_matter_bool(!attr_val_to_bool(&val));
    attribute::update(endpoint_id, cluster_id, attribute_id, &val);
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    esp_err_t err = ESP_OK;
    if (endpoint_id == room_air_conditioner_endpoint_id) {
        if (s_syncing_power_mode_attributes && (cluster_id == Thermostat::Id || cluster_id == OnOff::Id)) {
            return ESP_OK;
        }
        if (s_syncing_fan_attributes && cluster_id == FanControl::Id) {
            return ESP_OK;
        }

        if (cluster_id == OnOff::Id) {
            switch (attribute_id) {
                case OnOff::Attributes::OnOff::Id:
                    ESP_LOGI(TAG, "OnOff write received: %s", attr_val_to_bool(val) ? "ON" : "OFF");
                    err = app_driver_room_air_conditioner_set_power(val);
                    break;
                default:
                    ESP_LOGI(TAG, "Unhandled OnOff attribute: 0x%lx", attribute_id);
                    break;
            }
        } else if (cluster_id == Thermostat::Id) {
            switch (attribute_id) {
                case Thermostat::Attributes::SystemMode::Id:
                    ESP_LOGI(TAG, "SystemMode write received: %u", (unsigned)attr_val_to_uint8(val));
                    err = app_driver_room_air_conditioner_set_thermostat_mode(val);
                    break;
                case Thermostat::Attributes::OccupiedCoolingSetpoint::Id:
                case Thermostat::Attributes::OccupiedHeatingSetpoint::Id:
                    err = app_driver_room_air_conditioner_set_temperature(val);
                    break;
                default:
                    ESP_LOGI(TAG, "Unhandled thermostat attribute: 0x%lx", attribute_id);
                    break;
            }
        } else if (cluster_id == FanControl::Id) {
            switch (attribute_id) {
                case FanControl::Attributes::FanMode::Id:
                    ESP_LOGI(TAG, "FanMode write received: %u", static_cast<unsigned>(attr_val_to_uint8(val)));
                    err = app_driver_room_air_conditioner_set_fan_mode(val);
                    break;
                case FanControl::Attributes::PercentSetting::Id:
                    {
                        char value_buf[8];
                        ESP_LOGI(TAG, "Fan PercentSetting write received: %s",
                                 attr_val_uint8_to_log_str(val, value_buf, sizeof(value_buf)));
                    }
                    err = app_driver_room_air_conditioner_set_fan_percent(val);
                    break;
                case FanControl::Attributes::SpeedSetting::Id:
                    {
                        char value_buf[8];
                        ESP_LOGI(TAG, "Fan SpeedSetting write received: %s",
                                 attr_val_uint8_to_log_str(val, value_buf, sizeof(value_buf)));
                    }
                    err = app_driver_room_air_conditioner_set_fan_speed(val);
                    break;
                case FanControl::Attributes::RockSetting::Id:
                    ESP_LOGI(TAG, "Fan RockSetting write received: 0x%02x",
                             static_cast<unsigned>(attr_val_to_uint8(val)));
                    err = app_driver_room_air_conditioner_set_rock_setting(val);
                    break;
                default:
                    ESP_LOGI(TAG, "Unhandled FanControl attribute: 0x%lx", attribute_id);
                    break;
            }
        } else {
            ESP_LOGI(TAG, "Unhandled cluster: 0x%lx", cluster_id);
        }
    }
    return err;
}

esp_err_t app_driver_room_air_conditioner_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = ESP_OK;
    esp_matter_attr_val_t val = esp_matter_invalid(NULL);

    /* Setting thermostat defaults */
    val = esp_matter_enum8(4); // CoolingAndHeating; Heat is remapped to AC dry mode for HA compatibility.
    set_attribute_without_callbacks(endpoint_id, Thermostat::Id, Thermostat::Attributes::ControlSequenceOfOperation::Id,
                                    &val);

    val = esp_matter_enum8(DEFAULT_AC_MODE);
    set_attribute_without_callbacks(endpoint_id, Thermostat::Id, Thermostat::Attributes::SystemMode::Id, &val);
    ac_state.mode = (ac_mode_t)DEFAULT_AC_MODE;
    s_last_active_mode = ac_state.mode == AC_MODE_OFF ? AC_MODE_COOL : ac_state.mode;
    ac_state.power_on = ac_state.mode != AC_MODE_OFF;

    val = esp_matter_bool(ac_state.power_on);
    set_attribute_without_callbacks(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);

    val = esp_matter_int16(DEFAULT_TEMPERATURE);
    set_attribute_without_callbacks(endpoint_id, Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id,
                                    &val);
    set_attribute_without_callbacks(endpoint_id, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id,
                                    &val);

    update_reported_temperature();
    sync_thermostat_operating_state();
    sync_fan_control_attributes();
    app_driver_temperature_sensor_start();

    return err;
}

app_driver_handle_t app_driver_room_air_conditioner_init()
{
    if (trotec_3550_ir_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Trotec 3550 IR transmitter");
    }

    return NULL;
}

app_driver_handle_t app_driver_button_init()
{
    /* Initialize button */
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = {
        .gpio_num = kButtonGpio,
        .active_level = 0,
    };

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    iot_button_register_cb(handle, BUTTON_PRESS_DOWN, NULL, app_driver_button_toggle_cb, NULL);
    return (app_driver_handle_t)handle;
}
