#pragma once

#include <esp_err.h>

#include "app_priv.h"

esp_err_t trotec_3550_ir_init(void);
esp_err_t trotec_3550_ir_send_state(const ac_device_state_t *state);
