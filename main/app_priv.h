/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

/** Default attribute values used during initialization */
#define DEFAULT_POWER true
#define DEFAULT_TEMPERATURE 2400  // 24.0°C (in 0.01°C units)
#define MIN_TEMPERATURE 1600      // 16.0°C
#define MAX_TEMPERATURE 3000      // 30.0°C (Trotec 3550 protocol limit)
#define DEFAULT_FAN_SPEED 2       // Medium IR fan speed, not exposed over Matter
#define DEFAULT_AC_MODE 3         // Cool mode (Matter SystemMode::kCool)
#define DEFAULT_FAN_SWING false   // IR fan swing off, not exposed over Matter
#define FAN_SWING_UP_DOWN 0x02    // Native IR swing flag
#define DS18B20_GPIO 2            // External local-temperature sensor data pin
#define DS18B20_UPDATE_INTERVAL_MS 10000

/** AC Mode definitions */
typedef enum {
    AC_MODE_OFF = 0,         // Thermostat::SystemMode::kOff
    AC_MODE_COOL = 3,        // Thermostat::SystemMode::kCool
    AC_MODE_FAN = 7,         // Supported by IR encoder, not exposed over Matter
    AC_MODE_DEHUMIDIFY = 8   // Supported by IR encoder, not exposed over Matter
} ac_mode_t;

/** Fan Speed definitions */
typedef enum {
    FAN_SPEED_LOW = 1,
    FAN_SPEED_MEDIUM = 2,
    FAN_SPEED_HIGH = 3
} fan_speed_t;

/** AC device state structure */
typedef struct {
    bool power_on;
    ac_mode_t mode;
    fan_speed_t fan_speed;
    bool fan_swing;
    int16_t temperature;  // in 0.01°C units
} ac_device_state_t;

typedef void *app_driver_handle_t;

/** Initialize the room_air_conditioner driver
 *
 * This initializes the room_air_conditioner driver associated with the selected board.
 *
 * @return Handle on success.
 * @return NULL in case of failure.
 */
app_driver_handle_t app_driver_room_air_conditioner_init();

/** Initialize the button driver
 *
 * This initializes the button driver associated with the selected board.
 *
 * @return Handle on success.
 * @return NULL in case of failure.
 */
app_driver_handle_t app_driver_button_init();

/** Driver Update
 *
 * This API should be called to update the driver for the attribute being updated.
 * This is usually called from the common `app_attribute_update_cb()`.
 *
 * @param[in] endpoint_id Endpoint ID of the attribute.
 * @param[in] cluster_id Cluster ID of the attribute.
 * @param[in] attribute_id Attribute ID of the attribute.
 * @param[in] val Pointer to `esp_matter_attr_val_t`. Use appropriate elements as per the value type.
 *
 * @return ESP_OK on success.
 * @return error in case of failure.
 */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val);
void app_driver_note_pre_update_result(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, esp_err_t err);
bool app_driver_was_pre_update_handled(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id);

/** Set defaults for room_air_conditioner driver
 *
 * Set the attribute drivers to their default values from the created data model.
 *
 * @param[in] endpoint_id Endpoint ID of the driver.
 *
 * @return ESP_OK on success.
 * @return error in case of failure.
 */
esp_err_t app_driver_room_air_conditioner_set_defaults(uint16_t endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif
