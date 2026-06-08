/*
   Trotec 3550 IR transmitter.

   The protocol sends the complete A/C state in every 72-bit frame.
*/

#include "trotec_3550_ir.h"

#include <string.h>

#include <algorithm>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

static const char *TAG = "trotec_3550_ir";

static constexpr gpio_num_t kIrTxGpio = GPIO_NUM_21;

static constexpr size_t kFrameBytes = 9;

static constexpr uint16_t kHeaderMarkUs = 12005;
static constexpr uint16_t kHeaderSpaceUs = 5130;
static constexpr uint16_t kBitMarkUs = 545;
static constexpr uint16_t kOneSpaceUs = 1950;
static constexpr uint16_t kZeroSpaceUs = 500;
static constexpr uint32_t kMessageGapUs = 100000;
static constexpr uint32_t kCarrierPeriodUs = 26;
static constexpr uint32_t kCarrierHighUs = 9;

static bool s_gpio_initialized;

static uint8_t calc_checksum(const uint8_t *frame)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < kFrameBytes - 1; ++i) {
        sum += frame[i];
    }
    return sum;
}

static uint8_t matter_mode_to_trotec(ac_mode_t mode)
{
    switch (mode) {
        case AC_MODE_DEHUMIDIFY:
            return 2;
        case AC_MODE_FAN:
            return 3;
        case AC_MODE_OFF:
        case AC_MODE_COOL:
        default:
            return 1;
    }
}

static uint8_t temperature_c_to_f_offset(uint8_t temp_c)
{
    uint8_t temp_f = static_cast<uint8_t>((temp_c * 9) / 5 + 32);
    return static_cast<uint8_t>(temp_f - 59);
}

static void build_frame(const ac_device_state_t *state, uint8_t *frame)
{
    static const uint8_t kResetFrame[kFrameBytes] = {
        0x55, 0x60, 0x00, 0x0D, 0x00, 0x00, 0x10, 0x88, 0x5A,
    };

    memcpy(frame, kResetFrame, kFrameBytes);

    uint8_t temp_c = static_cast<uint8_t>(state->temperature / 100);
    temp_c = std::max<uint8_t>(16, std::min<uint8_t>(30, temp_c));

    frame[1] = static_cast<uint8_t>((state->fan_swing ? 0x01 : 0x00) |
                                    (state->power_on ? 0x02 : 0x00) |
                                    ((temp_c - 16) << 4));
    frame[2] = 0x00;
    frame[3] = temperature_c_to_f_offset(temp_c) & 0x1F;
    frame[6] = static_cast<uint8_t>((frame[6] & 0xCC) |
                                    matter_mode_to_trotec(state->mode) |
                                    ((state->fan_speed & 0x03) << 4));
    frame[7] = static_cast<uint8_t>((frame[7] & 0x7F) | 0x80);
    frame[8] = calc_checksum(frame);
}

static void ir_mark(uint32_t time_us)
{
    uint32_t cycles = time_us / kCarrierPeriodUs;
    uint32_t remainder = time_us % kCarrierPeriodUs;

    for (uint32_t i = 0; i < cycles; ++i) {
        gpio_set_level(kIrTxGpio, 1);
        esp_rom_delay_us(kCarrierHighUs);
        gpio_set_level(kIrTxGpio, 0);
        esp_rom_delay_us(kCarrierPeriodUs - kCarrierHighUs);
    }

    if (remainder > 0) {
        gpio_set_level(kIrTxGpio, 1);
        esp_rom_delay_us(std::min<uint32_t>(remainder, kCarrierHighUs));
        gpio_set_level(kIrTxGpio, 0);
        if (remainder > kCarrierHighUs) {
            esp_rom_delay_us(remainder - kCarrierHighUs);
        }
    }
}

static void ir_space(uint32_t time_us)
{
    gpio_set_level(kIrTxGpio, 0);
    esp_rom_delay_us(time_us);
}

esp_err_t trotec_3550_ir_init(void)
{
    if (s_gpio_initialized) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << kIrTxGpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure IR GPIO failed");
    gpio_set_level(kIrTxGpio, 0);

    s_gpio_initialized = true;

    ESP_LOGI(TAG, "Sröme YPS-12C IR initialized on GPIO %d using manual 38kHz carrier",
             static_cast<int>(kIrTxGpio));
    return ESP_OK;
}

esp_err_t trotec_3550_ir_send_state(const ac_device_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(trotec_3550_ir_init(), TAG, "IR init failed");

    uint8_t frame[kFrameBytes] = {};
    build_frame(state, frame);

    ESP_LOGI(TAG, "TX state: power=%s mode=%u fan=%u swing=%u temp=%u.%02uC frame=%02X %02X %02X %02X %02X %02X %02X %02X %02X",
             state->power_on ? "on" : "off",
             static_cast<unsigned>(state->mode),
             static_cast<unsigned>(state->fan_speed),
             state->fan_swing ? 1U : 0U,
             static_cast<unsigned>(state->temperature / 100),
             static_cast<unsigned>(state->temperature % 100),
             frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7], frame[8]);

    portDISABLE_INTERRUPTS();

    ir_mark(kHeaderMarkUs);
    ir_space(kHeaderSpaceUs);

    for (size_t byte_index = 0; byte_index < kFrameBytes; ++byte_index) {
        uint8_t byte = frame[byte_index];
        for (int bit = 7; bit >= 0; --bit) {
            bool one = (byte & (1U << bit)) != 0;
            ir_mark(kBitMarkUs);
            ir_space(one ? kOneSpaceUs : kZeroSpaceUs);
        }
    }

    ir_mark(kBitMarkUs);
    ir_space(kMessageGapUs);

    portENABLE_INTERRUPTS();

    return ESP_OK;
}
