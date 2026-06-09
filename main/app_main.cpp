/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <inttypes.h>
#include <nvs_flash.h>
#include <string.h>

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>
#include <esp_matter_providers.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/support/CodeUtils.h>

#include <common_macros.h>
#include <app_priv.h>
#include <app_reset.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <platform/ESP32/ESP32Config.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Dnssd.h>
#include <app/server/Server.h>
#include <platform/PlatformManager.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

static const char *TAG = "app_main";
uint16_t room_air_conditioner_endpoint_id = 0;
uint16_t room_temperature_sensor_endpoint_id = 0;

static void print_commissioning_status()
{
    chip::FabricTable &fabricTable = chip::Server::GetInstance().GetFabricTable();
    uint8_t fabricCount = fabricTable.FabricCount();
    
    ESP_LOGI(TAG, "=== COMMISSIONING STATUS ===");
    ESP_LOGI(TAG, "Device commissioned: %s", (fabricCount > 0) ? "YES" : "NO");
    ESP_LOGI(TAG, "Number of fabrics: %d", fabricCount);
    
    if (fabricCount > 0) {
        ESP_LOGI(TAG, "Device is already commissioned and ready for control");
        for (auto &fabricInfo : fabricTable) {
            ESP_LOGI(TAG, "Fabric ID: 0x%llx, Node ID: 0x%llx", 
                     fabricInfo.GetFabricId(), fabricInfo.GetNodeId());
        }
    } else {
        ESP_LOGI(TAG, "Device is NOT commissioned - waiting for commissioning");
        
        chip::CommissioningWindowManager &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
        if (commissionMgr.IsCommissioningWindowOpen()) {
            ESP_LOGI(TAG, "Commissioning window is OPEN");
        } else {
            ESP_LOGI(TAG, "Commissioning window is CLOSED");
        }
    }
    ESP_LOGI(TAG, "===========================");
}

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

namespace esp_matter {
namespace attribute {
esp_err_t destroy(cluster_t *cluster, attribute_t *attribute);
} // namespace attribute
namespace cluster {
namespace global {
namespace attribute {
attribute_t *create_cluster_revision(cluster_t *cluster, uint16_t value);
attribute_t *create_feature_map(cluster_t *cluster, uint32_t value);
} // namespace attribute
} // namespace global
} // namespace cluster
} // namespace esp_matter

constexpr auto k_timeout_seconds = 300;
static constexpr uint16_t kRootEndpointId = 0;
static constexpr uint32_t kIcdManagementClusterId = 0x00000046;
static constexpr uint32_t kNetworkCommissioningConnectMaxTimeSecondsAttributeId = 0x00000003;
static constexpr int16_t kInitialLocalTemperature = 2000; // 20.0 C until the DS18B20 task reports a valid reading.
static constexpr uint16_t kThermostatRunningStateCool = 0x0002;
static char s_empty_string[] = "";
static char s_vendor_name[] = "Ströme";
static char s_product_name[] = "Ströme AC";
static char s_model_name[] = "YPS-12C";
static char s_product_url[] = "https://github.com/EpKouhia/strome-ir-matter";
static char s_serial_number[] = "strome-ir-matter-dev";
static constexpr uint32_t kVendorId = 0xFFF1;  // CSA test VID used with development attestation.
static constexpr uint32_t kProductId = 0x8000; // Development PID from sdkconfig.
static constexpr uint32_t kHardwareVersion = 1;
static constexpr uint32_t kSetupPasscodeBase = 10000000;
static constexpr uint32_t kSetupPasscodeRange = 89999999;
static constexpr uint32_t kSpake2pIterationCount = 1000;
static constexpr uint8_t kSpake2pSalt[] = "SPAKE2P Key Salt";
static constexpr gpio_num_t kRfSwitchEnableGpio = GPIO_NUM_3;
static constexpr gpio_num_t kRfSwitchSelectGpio = GPIO_NUM_14;
static constexpr gpio_num_t kPairingModeButtonGpio = GPIO_NUM_1;
static constexpr gpio_num_t kPairingModeLedGpio = GPIO_NUM_15;
static constexpr int kPairingModeButtonActiveLevel = 0;
static constexpr int kPairingModeLedOnLevel = 1;
static constexpr uint32_t kPairingModeReleaseWaitMs = 30000;
static constexpr uint32_t kPairingModePollMs = 50;
static constexpr uint32_t kPairingModeBlinkMs = 500;
static constexpr uint32_t kPairingModeFactoryResetHoldMs = 15000;
static constexpr uint32_t kPairingModeFactoryResetBlinkMs = 120;
static constexpr uint8_t kPairingModeFactoryResetBlinkCount = 5;

static volatile bool s_pairing_led_blinking = false;
static volatile bool s_boot_pairing_window_pending = false;
static TaskHandle_t s_pairing_led_task = nullptr;
static TaskHandle_t s_pairing_button_task = nullptr;
static TaskHandle_t s_boot_pairing_task = nullptr;

static esp_err_t configure_rf_switch_antenna()
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << kRfSwitchEnableGpio) | (1ULL << kRfSwitchSelectGpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure RF switch GPIOs, err=%d", err);
        return err;
    }

    /* GPIO3 must be low first to enable RF switch control. */
    gpio_set_level(kRfSwitchEnableGpio, 0);
    gpio_set_level(kRfSwitchSelectGpio, 1);

    ESP_LOGI(TAG, "RF switch configured: GPIO3=%d, GPIO14=%d (external antenna)",
             gpio_get_level(kRfSwitchEnableGpio),
             gpio_get_level(kRfSwitchSelectGpio));
    return ESP_OK;
}

static void set_pairing_mode_led(bool on)
{
    gpio_set_level(kPairingModeLedGpio, on ? kPairingModeLedOnLevel : !kPairingModeLedOnLevel);
}

static void pairing_mode_led_task(void *)
{
    bool led_on = false;

    while (true) {
        if (s_pairing_led_blinking) {
            led_on = !led_on;
            set_pairing_mode_led(led_on);
            vTaskDelay(pdMS_TO_TICKS(kPairingModeBlinkMs));
        } else {
            led_on = false;
            set_pairing_mode_led(false);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void start_pairing_mode_led()
{
    if (!s_pairing_led_task) {
        BaseType_t task_created = xTaskCreate(pairing_mode_led_task, "pairing_led", 2048, nullptr, 1,
                                              &s_pairing_led_task);
        if (task_created != pdPASS) {
            ESP_LOGE(TAG, "Failed to start pairing mode LED task");
            s_pairing_led_task = nullptr;
            return;
        }
    }

    s_pairing_led_blinking = true;
}

static void stop_pairing_mode_led()
{
    s_pairing_led_blinking = false;
    set_pairing_mode_led(false);
}

static void stop_pairing_mode_led_task()
{
    s_pairing_led_blinking = false;
    if (s_pairing_led_task) {
        vTaskDelete(s_pairing_led_task);
        s_pairing_led_task = nullptr;
    }
    set_pairing_mode_led(false);
}

static void flash_pairing_mode_led(uint8_t blink_count, uint32_t interval_ms)
{
    stop_pairing_mode_led_task();

    for (uint8_t i = 0; i < blink_count; ++i) {
        set_pairing_mode_led(true);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        set_pairing_mode_led(false);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

static esp_err_t configure_pairing_mode_io()
{
    gpio_config_t button_conf = {
        .pin_bit_mask = (1ULL << kPairingModeButtonGpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&button_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure pairing mode button GPIO%d, err=%d", kPairingModeButtonGpio, err);
        return err;
    }

    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << kPairingModeLedGpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&led_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure pairing mode LED GPIO%d, err=%d", kPairingModeLedGpio, err);
        return err;
    }

    set_pairing_mode_led(false);
    return ESP_OK;
}

static bool is_pairing_mode_button_pressed()
{
    return gpio_get_level(kPairingModeButtonGpio) == kPairingModeButtonActiveLevel;
}

static void open_pairing_window_from_boot_button()
{
    chip::CommissioningWindowManager &commission_mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);

    if (commission_mgr.IsCommissioningWindowOpen()) {
        ESP_LOGI(TAG, "Pairing mode requested; commissioning window is already open");
        start_pairing_mode_led();
        return;
    }

    CHIP_ERROR err = commission_mgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                            chip::CommissioningWindowAdvertisement::kAllSupported);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open boot-requested commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
        stop_pairing_mode_led();
        return;
    }

    ESP_LOGI(TAG, "Boot-requested pairing mode opened for %d seconds", k_timeout_seconds);
}

static void boot_pairing_mode_task(void *)
{
    uint32_t waited_ms = 0;

    while (is_pairing_mode_button_pressed() && waited_ms < kPairingModeReleaseWaitMs) {
        vTaskDelay(pdMS_TO_TICKS(kPairingModePollMs));
        waited_ms += kPairingModePollMs;
    }

    if (is_pairing_mode_button_pressed()) {
        ESP_LOGW(TAG, "Pairing mode button still held after %lu ms; opening commissioning window once anyway",
                 static_cast<unsigned long>(kPairingModeReleaseWaitMs));
    } else {
        ESP_LOGI(TAG, "Pairing mode button released; opening commissioning window");
    }

    open_pairing_window_from_boot_button();
    s_boot_pairing_window_pending = false;
    s_boot_pairing_task = nullptr;
    vTaskDelete(nullptr);
}

static void start_boot_pairing_mode_task()
{
    if (!s_boot_pairing_window_pending || s_boot_pairing_task) {
        return;
    }

    BaseType_t task_created = xTaskCreate(boot_pairing_mode_task, "boot_pairing", 4096, nullptr, 1,
                                          &s_boot_pairing_task);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start boot pairing mode task");
        s_boot_pairing_task = nullptr;
        s_boot_pairing_window_pending = false;
        stop_pairing_mode_led();
    }
}

static void pairing_mode_button_task(void *)
{
    uint32_t held_ms = 0;
    bool armed = !is_pairing_mode_button_pressed();

    if (!armed) {
        ESP_LOGI(TAG, "Pairing button monitor waiting for GPIO%d release before arming reset gesture",
                 kPairingModeButtonGpio);
    }

    while (true) {
        if (!armed) {
            if (!is_pairing_mode_button_pressed()) {
                armed = true;
                ESP_LOGI(TAG, "Pairing button reset gesture armed");
            }
            vTaskDelay(pdMS_TO_TICKS(kPairingModePollMs));
            continue;
        }

        if (is_pairing_mode_button_pressed()) {
            held_ms += kPairingModePollMs;
            if (held_ms == kPairingModePollMs) {
                ESP_LOGI(TAG, "Pairing button pressed after boot; hold for %lu ms to factory reset Matter data",
                         static_cast<unsigned long>(kPairingModeFactoryResetHoldMs));
            }

            if (held_ms >= kPairingModeFactoryResetHoldMs) {
                ESP_LOGW(TAG, "Pairing button held for %lu ms; factory resetting Matter commissioning data",
                         static_cast<unsigned long>(kPairingModeFactoryResetHoldMs));
                flash_pairing_mode_led(kPairingModeFactoryResetBlinkCount, kPairingModeFactoryResetBlinkMs);
                esp_err_t err = esp_matter::factory_reset();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Matter factory reset failed, err=%d", err);
                    set_pairing_mode_led(false);
                }
                s_pairing_button_task = nullptr;
                vTaskDelete(nullptr);
            }
        } else if (held_ms > 0) {
            ESP_LOGI(TAG, "Pairing button released before factory reset threshold");
            held_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(kPairingModePollMs));
    }
}

static void start_pairing_mode_button_monitor()
{
    if (s_pairing_button_task) {
        return;
    }

    BaseType_t task_created = xTaskCreate(pairing_mode_button_task, "pairing_btn", 3072, nullptr, 1,
                                          &s_pairing_button_task);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start pairing mode button monitor task");
        s_pairing_button_task = nullptr;
    }
}

static uint64_t get_factory_mac_id()
{
    uint8_t mac[6] = {};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read factory MAC for setup payload, err=%d", err);
        return 0;
    }

    uint64_t mac_id = 0;
    for (uint8_t byte : mac) {
        mac_id = (mac_id << 8) | byte;
    }
    return mac_id;
}

static uint32_t mix_mac_to_u32(uint64_t mac_id)
{
    uint32_t value = static_cast<uint32_t>(mac_id) ^ static_cast<uint32_t>(mac_id >> 32);
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

static uint32_t derive_setup_passcode()
{
    uint32_t passcode = kSetupPasscodeBase + (mix_mac_to_u32(get_factory_mac_id()) % kSetupPasscodeRange);
    while (!chip::SetupPayload::IsValidSetupPIN(passcode)) {
        passcode++;
        if (passcode > chip::kSetupPINCodeMaximumValue) {
            passcode = kSetupPasscodeBase;
        }
    }
    return passcode;
}

static uint16_t derive_setup_discriminator()
{
    return static_cast<uint16_t>(mix_mac_to_u32(get_factory_mac_id() ^ 0x5a5a5a5a5a5aULL) & 0x0FFF);
}

class StromeCommissionableDataProvider : public chip::DeviceLayer::CommissionableDataProvider {
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t &setupDiscriminator) override
    {
        setupDiscriminator = derive_setup_discriminator();
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetSetupDiscriminator(uint16_t) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t &iterationCount) override
    {
        iterationCount = kSpake2pIterationCount;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &saltBuf) override
    {
        chip::ByteSpan salt(kSpake2pSalt, sizeof(kSpake2pSalt) - 1);
        VerifyOrReturnError(salt.size() <= saltBuf.size(), CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(saltBuf.data(), salt.data(), salt.size());
        saltBuf.reduce_size(salt.size());
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &verifierBuf, size_t &verifierLen) override
    {
        chip::Crypto::Spake2pVerifier verifier;
        chip::ByteSpan salt(kSpake2pSalt, sizeof(kSpake2pSalt) - 1);
        ReturnErrorOnFailure(verifier.Generate(kSpake2pIterationCount, salt, derive_setup_passcode()));
        ReturnErrorOnFailure(verifier.Serialize(verifierBuf));
        verifierLen = verifierBuf.size();
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSetupPasscode(uint32_t &setupPasscode) override
    {
        setupPasscode = derive_setup_passcode();
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR SetSetupPasscode(uint32_t) override
    {
        return CHIP_ERROR_NOT_IMPLEMENTED;
    }
};

static StromeCommissionableDataProvider s_commissionable_data_provider;

class StromeDeviceInstanceInfoProvider : public chip::DeviceLayer::DeviceInstanceInfoProvider {
public:
    CHIP_ERROR GetVendorName(char *buf, size_t bufSize) override
    {
        return copy_string(buf, bufSize, s_vendor_name);
    }

    CHIP_ERROR GetVendorId(uint16_t &vendorId) override
    {
        vendorId = static_cast<uint16_t>(kVendorId);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetProductName(char *buf, size_t bufSize) override
    {
        return copy_string(buf, bufSize, s_product_name);
    }

    CHIP_ERROR GetProductId(uint16_t &productId) override
    {
        productId = static_cast<uint16_t>(kProductId);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetPartNumber(char *buf, size_t bufSize) override
    {
        return copy_string(buf, bufSize, s_model_name);
    }

    CHIP_ERROR GetProductURL(char *buf, size_t bufSize) override
    {
        return copy_string(buf, bufSize, s_product_url);
    }

    CHIP_ERROR GetProductLabel(char *buf, size_t bufSize) override
    {
        return copy_string(buf, bufSize, s_model_name);
    }

    CHIP_ERROR GetSerialNumber(char *buf, size_t bufSize) override
    {
        return copy_string(buf, bufSize, s_serial_number);
    }

    CHIP_ERROR GetManufacturingDate(uint16_t &year, uint8_t &month, uint8_t &day) override
    {
        year = 2026;
        month = 6;
        day = 9;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetHardwareVersion(uint16_t &hardwareVersion) override
    {
        hardwareVersion = static_cast<uint16_t>(kHardwareVersion);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetHardwareVersionString(char *buf, size_t bufSize) override
    {
        return copy_string(buf, bufSize, s_model_name);
    }

    CHIP_ERROR GetRotatingDeviceIdUniqueId(chip::MutableByteSpan &uniqueIdSpan) override
    {
        static constexpr uint8_t unique_id[] = {
            's', 't', 'r', 'o', 'm', 'e', '-', 'i',
            'r', '-', 'm', 'a', 't', 't', 'e', 'r',
        };
        VerifyOrReturnError(uniqueIdSpan.size() >= sizeof(unique_id), CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(uniqueIdSpan.data(), unique_id, sizeof(unique_id));
        uniqueIdSpan.reduce_size(sizeof(unique_id));
        return CHIP_NO_ERROR;
    }

private:
    static CHIP_ERROR copy_string(char *buf, size_t bufSize, const char *value)
    {
        size_t length = strlen(value);
        VerifyOrReturnError(bufSize > length, CHIP_ERROR_BUFFER_TOO_SMALL);
        memcpy(buf, value, length + 1);
        return CHIP_NO_ERROR;
    }
};

static StromeDeviceInstanceInfoProvider s_device_instance_info_provider;

static void set_factory_string_if_needed(chip::DeviceLayer::Internal::ESP32Config::Key key, const char *name,
                                         const char *value)
{
    char current_value[96] = {};
    size_t current_len = 0;
    CHIP_ERROR err = chip::DeviceLayer::Internal::ESP32Config::ReadConfigValueStr(key, current_value,
                                                                                   sizeof(current_value), current_len);
    if (err == CHIP_NO_ERROR && strcmp(current_value, value) == 0) {
        return;
    }

    err = chip::DeviceLayer::Internal::ESP32Config::WriteConfigValueStr(key, value);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Matter factory data: %s = %s", name, value);
    } else {
        ESP_LOGW(TAG, "Failed to write Matter factory data %s: %" CHIP_ERROR_FORMAT, name, err.Format());
    }
}

static void set_factory_u32_if_needed(chip::DeviceLayer::Internal::ESP32Config::Key key, const char *name,
                                      uint32_t value)
{
    uint32_t current_value = 0;
    CHIP_ERROR err = chip::DeviceLayer::Internal::ESP32Config::ReadConfigValue(key, current_value);
    if (err == CHIP_NO_ERROR && current_value == value) {
        return;
    }

    err = chip::DeviceLayer::Internal::ESP32Config::WriteConfigValue(key, value);
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Matter factory data: %s = %" PRIu32, name, value);
    } else {
        ESP_LOGW(TAG, "Failed to write Matter factory data %s: %" CHIP_ERROR_FORMAT, name, err.Format());
    }
}

static void configure_matter_factory_data()
{
    using chip::DeviceLayer::Internal::ESP32Config;

    set_factory_u32_if_needed(ESP32Config::kConfigKey_VendorId, "VendorId", kVendorId);
    set_factory_string_if_needed(ESP32Config::kConfigKey_VendorName, "VendorName", s_vendor_name);
    set_factory_u32_if_needed(ESP32Config::kConfigKey_ProductId, "ProductId", kProductId);
    set_factory_string_if_needed(ESP32Config::kConfigKey_ProductName, "ProductName", s_product_name);
    set_factory_u32_if_needed(ESP32Config::kConfigKey_HardwareVersion, "HardwareVersion", kHardwareVersion);
    set_factory_string_if_needed(ESP32Config::kConfigKey_ProductLabel, "ProductLabel", s_model_name);
    set_factory_string_if_needed(ESP32Config::kConfigKey_PartNumber, "PartNumber", s_model_name);
    set_factory_string_if_needed(ESP32Config::kConfigKey_HardwareVersionString, "HardwareVersionString", s_model_name);
    set_factory_string_if_needed(ESP32Config::kConfigKey_ProductURL, "ProductURL", s_product_url);
    set_factory_string_if_needed(ESP32Config::kConfigKey_SerialNum, "SerialNumber", s_serial_number);
}

static attribute_t *create_static_attribute_if_missing(cluster_t *cluster, uint32_t attribute_id, uint16_t flags,
                                                       esp_matter_attr_val_t val)
{
    attribute_t *attribute = attribute::get(cluster, attribute_id);
    if (attribute) {
        return attribute;
    }
    return attribute::create(cluster, attribute_id, flags, val);
}

static attribute_t *create_static_attribute_if_missing(cluster_t *cluster, uint32_t attribute_id, esp_matter_attr_val_t val)
{
    return create_static_attribute_if_missing(cluster, attribute_id, ATTRIBUTE_FLAG_NONE, val);
}

static attribute_t *create_or_replace_attribute(cluster_t *cluster, uint32_t attribute_id, uint8_t flags,
                                                esp_matter_attr_val_t val)
{
    attribute_t *attribute = attribute::get(cluster, attribute_id);
    if (attribute) {
        attribute::destroy(cluster, attribute);
    }
    return attribute::create(cluster, attribute_id, flags, val);
}

static void set_or_create_string_attribute(cluster_t *cluster, uint32_t attribute_id, char *value)
{
    esp_matter_attr_val_t val = esp_matter_char_str(value, strlen(value));
    attribute_t *attribute = attribute::get(cluster, attribute_id);
    if (attribute) {
        attribute::destroy(cluster, attribute);
    }
    attribute::create(cluster, attribute_id, ATTRIBUTE_FLAG_NONE, val);
}

static void add_root_compatibility_attributes(endpoint_t *root_endpoint)
{
    cluster_t *basic_information_cluster = cluster::get(root_endpoint, BasicInformation::Id);
    if (basic_information_cluster) {
        set_or_create_string_attribute(basic_information_cluster, BasicInformation::Attributes::VendorName::Id,
                                       s_vendor_name);
        set_or_create_string_attribute(basic_information_cluster, BasicInformation::Attributes::ProductName::Id,
                                       s_product_name);
        set_or_create_string_attribute(basic_information_cluster, BasicInformation::Attributes::NodeLabel::Id,
                                       s_product_name);
        set_or_create_string_attribute(basic_information_cluster, BasicInformation::Attributes::HardwareVersionString::Id,
                                       s_model_name);
        set_or_create_string_attribute(basic_information_cluster, BasicInformation::Attributes::ProductLabel::Id,
                                       s_model_name);
        set_or_create_string_attribute(basic_information_cluster, BasicInformation::Attributes::PartNumber::Id,
                                       s_model_name);
    }

    cluster_t *network_commissioning_cluster = cluster::get(root_endpoint, NetworkCommissioning::Id);
    if (network_commissioning_cluster) {
        attribute_t *connect_max_time_seconds =
            attribute::get(network_commissioning_cluster, kNetworkCommissioningConnectMaxTimeSecondsAttributeId);
        if (connect_max_time_seconds &&
            (attribute::get_flags(connect_max_time_seconds) & ATTRIBUTE_FLAG_MANAGED_INTERNALLY)) {
            attribute::destroy(network_commissioning_cluster, connect_max_time_seconds);
            connect_max_time_seconds = nullptr;
        }
        if (!connect_max_time_seconds) {
            attribute::create(network_commissioning_cluster, kNetworkCommissioningConnectMaxTimeSecondsAttributeId,
                              ATTRIBUTE_FLAG_NONE, esp_matter_uint8(30));
        }
    }

    cluster_t *icd_management_cluster = cluster::get(root_endpoint, kIcdManagementClusterId);
    if (!icd_management_cluster) {
        icd_management_cluster = cluster::create(root_endpoint, kIcdManagementClusterId, CLUSTER_FLAG_SERVER);
    }
    if (icd_management_cluster) {
        create_static_attribute_if_missing(icd_management_cluster, 0x00000000, esp_matter_uint32(0));
        create_static_attribute_if_missing(icd_management_cluster, 0x00000001, esp_matter_uint32(0));
        create_static_attribute_if_missing(icd_management_cluster, 0x00000002, esp_matter_uint16(0));
        create_static_attribute_if_missing(icd_management_cluster, 0x00000006, esp_matter_bitmap32(0));
        create_static_attribute_if_missing(icd_management_cluster, 0x00000007, esp_matter_char_str(s_empty_string, 0));
        create_static_attribute_if_missing(icd_management_cluster, 0x0000FFFC, esp_matter_bitmap32(0));
        create_static_attribute_if_missing(icd_management_cluster, 0x0000FFFD, esp_matter_uint16(1));
    }
}

static void add_thermostat_compatibility_clusters(endpoint_t *endpoint)
{
    cluster_t *thermostat_cluster = cluster::get(endpoint, Thermostat::Id);
    if (thermostat_cluster) {
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::AbsMinCoolSetpointLimit::Id,
                                    ATTRIBUTE_FLAG_NONE, esp_matter_int16(MIN_TEMPERATURE));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::AbsMaxCoolSetpointLimit::Id,
                                    ATTRIBUTE_FLAG_NONE, esp_matter_int16(MAX_TEMPERATURE));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::MinCoolSetpointLimit::Id,
                                    ATTRIBUTE_FLAG_NONVOLATILE | ATTRIBUTE_FLAG_WRITABLE,
                                    esp_matter_int16(MIN_TEMPERATURE));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::MaxCoolSetpointLimit::Id,
                                    ATTRIBUTE_FLAG_NONVOLATILE | ATTRIBUTE_FLAG_WRITABLE,
                                    esp_matter_int16(MAX_TEMPERATURE));
        attribute_t *cooling_setpoint =
            create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::OccupiedCoolingSetpoint::Id,
                                        ATTRIBUTE_FLAG_NONVOLATILE | ATTRIBUTE_FLAG_WRITABLE,
                                        esp_matter_int16(DEFAULT_TEMPERATURE));
        if (cooling_setpoint) {
            attribute::add_bounds(cooling_setpoint, esp_matter_int16(MIN_TEMPERATURE),
                                  esp_matter_int16(MAX_TEMPERATURE));
        }
        /*
         * RainMaker's Matter AC UI probes both thermostat setpoint paths before
         * enabling the temperature slider. Keep heating mode unsupported, but
         * mirror this attribute to the same stored temperature so controller UIs
         * can expose a writable setpoint.
         */
        attribute_t *heating_setpoint =
            create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::OccupiedHeatingSetpoint::Id,
                                        ATTRIBUTE_FLAG_NONVOLATILE | ATTRIBUTE_FLAG_WRITABLE,
                                        esp_matter_int16(DEFAULT_TEMPERATURE));
        if (heating_setpoint) {
            attribute::add_bounds(heating_setpoint, esp_matter_int16(MIN_TEMPERATURE),
                                  esp_matter_int16(MAX_TEMPERATURE));
        }
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::PICoolingDemand::Id,
                                    ATTRIBUTE_FLAG_NONE, esp_matter_uint8(DEFAULT_POWER ? 50 : 0));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::RemoteSensing::Id,
                                    ATTRIBUTE_FLAG_NONVOLATILE | ATTRIBUTE_FLAG_WRITABLE, esp_matter_bitmap8(0));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::ThermostatRunningMode::Id,
                                    ATTRIBUTE_FLAG_NONE, esp_matter_enum8(DEFAULT_POWER ? DEFAULT_AC_MODE : AC_MODE_OFF));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::ThermostatRunningState::Id,
                                    ATTRIBUTE_FLAG_NONE,
                                    esp_matter_bitmap16(DEFAULT_POWER ? kThermostatRunningStateCool : 0));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::TemperatureSetpointHold::Id,
                                    ATTRIBUTE_FLAG_NONVOLATILE | ATTRIBUTE_FLAG_WRITABLE, esp_matter_enum8(0));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::ThermostatProgrammingOperationMode::Id,
                                    ATTRIBUTE_FLAG_NONVOLATILE | ATTRIBUTE_FLAG_WRITABLE, esp_matter_bitmap8(0));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::SetpointChangeSource::Id,
                                    ATTRIBUTE_FLAG_NONE, esp_matter_enum8(0));
        create_or_replace_attribute(thermostat_cluster, Thermostat::Attributes::SetpointChangeAmount::Id,
                                    ATTRIBUTE_FLAG_NULLABLE, esp_matter_nullable_int16(nullable<int16_t>()));
    }

    if (!cluster::get(endpoint, TemperatureMeasurement::Id)) {
        cluster::temperature_measurement::config_t temperature_config;
        temperature_config.measured_value = nullable<int16_t>(kInitialLocalTemperature);
        temperature_config.min_measured_value = nullable<int16_t>(MIN_TEMPERATURE);
        temperature_config.max_measured_value = nullable<int16_t>(MAX_TEMPERATURE);
        cluster_t *temperature_cluster =
            cluster::temperature_measurement::create(endpoint, &temperature_config, CLUSTER_FLAG_SERVER);
        if (temperature_cluster) {
            ESP_LOGI(TAG, "Added optimistic Temperature Measurement cluster to Thermostat endpoint");
        } else {
            ESP_LOGE(TAG, "Failed to add Temperature Measurement cluster to Thermostat endpoint");
        }
    }

    if (!cluster::get(endpoint, ThermostatUserInterfaceConfiguration::Id)) {
        cluster::thermostat_user_interface_configuration::config_t ui_config;
        cluster_t *ui_cluster =
            cluster::thermostat_user_interface_configuration::create(endpoint, &ui_config, CLUSTER_FLAG_SERVER);
        if (ui_cluster) {
            ESP_LOGI(TAG, "Added Thermostat UI Configuration cluster to Thermostat endpoint");
        } else {
            ESP_LOGE(TAG, "Failed to add Thermostat UI Configuration cluster to Thermostat endpoint");
        }
    }
}

static void retry_operational_advertising(const char *reason)
{
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        return;
    }

    CHIP_ERROR err = chip::app::DnssdServer::Instance().AdvertiseOperational();
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "Operational advertising refreshed after %s", reason);
    } else {
        ESP_LOGW(TAG, "Operational advertising retry after %s failed: %" CHIP_ERROR_FORMAT, reason, err.Format());
    }
}

static void delayed_operational_advertising_retry(chip::System::Layer *, void *app_state)
{
    retry_operational_advertising(static_cast<const char *>(app_state));
}

static void schedule_operational_advertising_retry(chip::System::Clock::Timeout delay, const char *reason)
{
    CHIP_ERROR err = chip::DeviceLayer::SystemLayer().StartTimer(delay, delayed_operational_advertising_retry,
                                                                 const_cast<char *>(reason));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Failed to schedule operational advertising retry after %s: %" CHIP_ERROR_FORMAT, reason,
                 err.Format());
    }
}

static void refresh_operational_advertising(const char *reason)
{
    retry_operational_advertising(reason);
    schedule_operational_advertising_retry(chip::System::Clock::Seconds16(2), "2s delay");
    schedule_operational_advertising_retry(chip::System::Clock::Seconds16(6), "6s delay");
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        refresh_operational_advertising("IP address change");
        break;

    case chip::DeviceLayer::DeviceEventType::kThreadConnectivityChange:
        ESP_LOGI(TAG, "Thread connectivity changed: %d", static_cast<int>(event->ThreadConnectivityChange.Result));
        if (event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established) {
            refresh_operational_advertising("Thread connectivity established");
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "🎉 COMMISSIONING COMPLETED SUCCESSFULLY! 🎉");
        stop_pairing_mode_led();
        print_commissioning_status();
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "❌ COMMISSIONING FAILED - Fail safe timer expired");
        stop_pairing_mode_led();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "🔄 COMMISSIONING SESSION STARTED");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "⏹️ COMMISSIONING SESSION STOPPED");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "🚪 COMMISSIONING WINDOW OPENED - Device ready for commissioning");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "🚪 COMMISSIONING WINDOW CLOSED");
        stop_pairing_mode_led();
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        {
            ESP_LOGI(TAG, "🗑️ FABRIC REMOVED - Device decommissioned");
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0)
            {
                ESP_LOGI(TAG, "Last fabric removed - reopening commissioning window");
                chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
                constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
                if (!commissionMgr.IsCommissioningWindowOpen())
                {
                    /* After removing the last fabric, keep network credentials and advertise over DNS-SD. */
                    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
                    if (err != CHIP_NO_ERROR)
                    {
                        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                    } else {
                        ESP_LOGI(TAG, "Commissioning window reopened for new pairing");
                    }
                }
            }
            print_commissioning_status();
        break;
        }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "⚠️ FABRIC WILL BE REMOVED");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "🔄 FABRIC UPDATED");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "✅ FABRIC COMMITTED");
        refresh_operational_advertising("fabric commit");
        break;
    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "Matter attribute callback: endpoint=%u cluster=0x%lx attribute=0x%lx type=%d value_type=%d",
             endpoint_id, cluster_id, attribute_id, type, val ? val->type : -1);

    app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
    if (type == PRE_UPDATE) {
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
        app_driver_note_pre_update_result(endpoint_id, cluster_id, attribute_id, err);
    } else if (type == POST_UPDATE && (cluster_id == Thermostat::Id || cluster_id == OnOff::Id) &&
               !app_driver_was_pre_update_handled(endpoint_id, cluster_id, attribute_id)) {
        ESP_LOGI(TAG, "Handling AC attribute from POST_UPDATE fallback");
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "🚀 MATTER APP STARTING UP");
    ESP_LOGI(TAG, "📱 Ströme AC Controller - Version 1.0");
    ESP_LOGI(TAG, "🏷️ Model: YPS-12C");
    ESP_LOGI(TAG, "🏠 Device Type: Room Air Conditioner (Always-On IR AC bridge)");
    ESP_LOGI(TAG, "⚙️ Features: Power, Cool/Off mode, Temperature setpoint");
    ESP_LOGI(TAG, "🔌 Power Type: Mains-Powered (No ICD/Battery Management Required)");
    ESP_LOGI(TAG, "🚫 ICD Server: Disabled - always-on device");

    err = configure_rf_switch_antenna();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to configure RF switch antenna, err:%d", err));

    err = configure_pairing_mode_io();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to configure pairing mode GPIOs, err:%d", err));

    if (is_pairing_mode_button_pressed()) {
        s_boot_pairing_window_pending = true;
        ESP_LOGI(TAG, "Pairing mode button GPIO%d held low at boot; startup will continue",
                 kPairingModeButtonGpio);
        start_pairing_mode_led();
    }

    /* Initialize the ESP NVS layer */
    nvs_flash_init();
    configure_matter_factory_data();

    /* Initialize driver */
    app_driver_handle_t room_air_conditioner_handle = app_driver_room_air_conditioner_init();
    app_driver_handle_t button_handle = app_driver_button_init();
    app_reset_button_register(button_handle);

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    endpoint_t *root_endpoint = endpoint::get(node, kRootEndpointId);
    ABORT_APP_ON_FAILURE(root_endpoint != nullptr, ESP_LOGE(TAG, "Failed to get root node endpoint"));
    add_root_compatibility_attributes(root_endpoint);

    esp_matter::endpoint::room_air_conditioner::config_t ac_config = {};
    ac_config.on_off.on_off = DEFAULT_POWER;
    ac_config.thermostat.local_temperature = nullable<int16_t>(kInitialLocalTemperature);
    ac_config.thermostat.feature_flags = cluster::thermostat::feature::cooling::get_id();
    ac_config.thermostat.control_sequence_of_operation = 0; // Cooling only
    ac_config.thermostat.system_mode = DEFAULT_AC_MODE;
    ac_config.thermostat.features.cooling.occupied_cooling_setpoint = DEFAULT_TEMPERATURE;

    endpoint_t *endpoint = esp_matter::endpoint::room_air_conditioner::create(node, &ac_config, ENDPOINT_FLAG_NONE,
                                                                              room_air_conditioner_handle);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create Room Air Conditioner endpoint"));
    add_thermostat_compatibility_clusters(endpoint);

    room_air_conditioner_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Room Air Conditioner IR bridge created with endpoint_id %d", room_air_conditioner_endpoint_id);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

    /* This must be registered before esp_matter::start() so QR/manual code and PASE use the same passcode. */
    esp_matter::set_custom_commissionable_data_provider(&s_commissionable_data_provider);
#if CONFIG_CUSTOM_DEVICE_INSTANCE_INFO_PROVIDER
    esp_matter::set_custom_device_instance_info_provider(&s_device_instance_info_provider);
#endif

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    /* Print startup commissioning status */
    ESP_LOGI(TAG, "🔍 INITIAL DEVICE STATUS:");
    print_commissioning_status();

    /* Print commissioning information */
    chip::BitFlags<chip::RendezvousInformationFlag> rendezvous(chip::RendezvousInformationFlag::kBLE);
    PrintOnboardingCodes(rendezvous);

    start_boot_pairing_mode_task();

    start_pairing_mode_button_monitor();

    /* Starting driver with default values */
    app_driver_room_air_conditioner_set_defaults(room_air_conditioner_endpoint_id);

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif
}
