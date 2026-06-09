# ESP32-Matter AC Controller - Setup and Testing Guide

## Prerequisites Setup

### 1. Install ESP-IDF (v5.4.1 or later)
```bash
# Clone ESP-IDF
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
git checkout v5.4.1
git submodule update --init --recursive
./install.sh
cd ..
```

### 2. Install ESP-Matter SDK
```bash
# Source ESP-IDF first
cd esp-idf
source ./export.sh
cd ..

# Clone ESP-Matter (shallow clone for faster download)
git clone --depth 1 https://github.com/espressif/esp-matter.git
cd esp-matter
git submodule update --init --depth 1
cd ./connectedhomeip/connectedhomeip

# For Linux
./scripts/checkout_submodules.py --platform esp32 linux --shallow

# For macOS
./scripts/checkout_submodules.py --platform esp32 darwin --shallow

cd ../..
./install.sh
cd ..
```

### 3. Environment Setup (run each time)
```bash
# Source ESP-IDF
cd esp-idf
source ./export.sh
cd ..

# Source ESP-Matter
cd esp-matter
source ./export.sh
cd ..

# Enable ccache for faster builds
export IDF_CCACHE_ENABLE=1

# Navigate to project
cd strome-ir-matter
```

## Build Process

### 1. Configure Target
```bash
idf.py set-target esp32c6
```

### 2. Build Project
```bash
idf.py build
```

Expected output on successful build:
```
Project build complete. To flash, run:
idf.py flash
```

### 3. Flash to Device
```bash
# Flash and start monitoring
idf.py flash monitor
```

## Testing the AC Controller

### Boot Sequence Verification

When the device boots, you should see logs like:
```
I (456) app_main: Room Air Conditioner IR bridge created with endpoint_id 1
I (567) app_driver: DS18B20 local temperature sensor initialized on GPIO2
I (670) app_driver: Ströme AC Power: ON
I (671) app_driver: Ströme AC Mode: COOL
I (672) app_driver: Temperature: 24.0°C
```

If D1 / GPIO1 is held low during boot, startup continues and the device opens a Matter pairing window after Matter starts and GPIO1 is released:
```
I (xxx) app_main: Pairing mode button GPIO1 held low at boot; startup will continue
I (xxx) app_main: Pairing mode button released; opening commissioning window
I (xxx) app_main: Boot-requested pairing mode opened for 300 seconds
```

GPIO15 blinks while this boot-requested pairing window is open and turns off when commissioning completes, fails, or the window closes.

After normal boot, holding D1 / GPIO1 for 15 seconds starts a Matter factory reset. GPIO15 fast-blinks five times, then the device reboots and returns to commissioning.

### Device Commissioning

#### 1. Install chip-tool (Matter Commissioner)
The chip-tool should be built as part of the ESP-Matter installation:
```bash
# Should be available at:
${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/out/host/chip-tool
```

#### 2. Commission the Device
```bash
# Replace placeholders with actual values
chip-tool pairing ble-thread <NODE_ID> hex:<THREAD_DATASET_HEX> <SETUP_PIN> <DISCRIMINATOR>

# Example using the setup PIN and discriminator printed by the device at boot:
chip-tool pairing ble-thread 12345 hex:0e080000000000010000000300000f35060004001fffe00208 <SETUP_PIN> <DISCRIMINATOR>
```

This ESP32-C6 build is configured as a Matter-over-Thread, mains-powered device:
`CONFIG_OPENTHREAD_ENABLED=y`, `CONFIG_THREAD_NETWORK_COMMISSIONING_DRIVER=y`,
`CONFIG_ENABLE_WIFI_STATION=n`, and `CONFIG_ENABLE_ICD_SERVER=n`.

#### 3. Verify Commissioning
```bash
chip-tool basicinformation read vendor-name 12345 0
```

### AC Control Testing

#### Test 1: Power Control
```bash
# Turn AC on
chip-tool onoff on 12345 1
# Expected log: I (xxx) app_driver: Ströme AC Power: ON

# Turn AC off
chip-tool onoff off 12345 1
# Expected log: I (xxx) app_driver: Ströme AC Power: OFF

# Read power status
chip-tool onoff read on-off 12345 1
```

#### Test 2: Mode Control
```bash
# Set off mode (SystemMode: Off=0)
chip-tool thermostat write system-mode 0 12345 1
# Expected log: I (xxx) app_driver: Ströme AC Mode: OFF

# Set cooling mode (SystemMode: Cool=3)
chip-tool thermostat write system-mode 3 12345 1
# Expected log: I (xxx) app_driver: Ströme AC Mode: COOL

# Unsupported modes are intentionally rejected in the first Room Air Conditioner phase.
chip-tool thermostat write system-mode 7 12345 1
chip-tool thermostat write system-mode 8 12345 1
# Expected log: W (xxx) app_driver: Rejected unsupported AC mode
```

#### Test 3: Temperature Control
```bash
# Valid range: 16.00°C to 30.00°C

# Set temperature to 22°C (2200 = 22.00°C in 0.01°C units)
chip-tool thermostat write occupied-cooling-setpoint 2200 12345 1
# Expected log: I (xxx) app_driver: Temperature: 22.0°C

# Set temperature to 26°C
chip-tool thermostat write occupied-cooling-setpoint 2600 12345 1
# Expected log: I (xxx) app_driver: Temperature: 26.0°C
```

#### Fan Speed and Swing
Fan speed and swing are not exposed in this first Room Air Conditioner phase. Verify that the server list does not include Fan Control (`0x0202`). These controls are planned for a follow-up after the minimal AC endpoint is stable.

### Complete AC Setup Test
```bash
# Full AC configuration sequence
chip-tool onoff on 12345 1                                      # Power ON
chip-tool thermostat write system-mode 3 12345 1                # Cooling mode
chip-tool thermostat write occupied-cooling-setpoint 2400 12345 1  # 24°C
```

Expected log sequence:
```
I (xxx) app_driver: Ströme AC Power: ON
I (xxx) app_driver: Ströme AC Mode: COOL
I (xxx) app_driver: Temperature: 24.0°C
```

## Troubleshooting

### Build Issues
1. **Missing ESP-IDF**: Ensure ESP-IDF v5.4.1+ is installed and sourced
2. **Missing ESP-Matter**: Ensure ESP-Matter is installed and sourced
3. **Submodule errors**: Run `git submodule update --init --recursive`
4. **Python errors**: Install required Python packages: `pip install -r $IDF_PATH/requirements.txt`

### Runtime Issues
1. **Device not booting**: Check serial connection and baud rate (115200)
2. **Commissioning fails**: Verify the Thread operational dataset, QR code/pairing info, and that the previous failed fabric was removed from the controller
3. **Commands not working**: Check node ID and endpoint number (usually 1 after recommissioning)
4. **No logs**: Ensure log level is set to INFO or DEBUG

### Verification Commands
```bash
# Check all clusters on endpoint 1
chip-tool descriptor read server-list 12345 1

# Should show clusters:
# - 0x0006 (OnOff)
# - 0x0201 (Thermostat)
# - 0x001D (Descriptor)
# It should not show 0x0202 (Fan Control) in the first Room Air Conditioner phase.
# It should not show 0x0046 (ICD Management) for this always-on controller.
```

## Hardware Integration

1. **Add IR Hardware**: Connect the IR LED driver circuit to GPIO21
2. **Add Local Temperature Sensor**: Connect DS18B20 data to GPIO2 with a 4.7 kOhm pull-up to 3.3 V
3. **Add Pairing / Reset Button**: Connect a normally-open momentary button between D1 / GPIO1 and GND. The internal pull-up is enabled in firmware.
4. **Verify Pairing Indicator**: Hold GPIO1 low during boot, release it after startup begins, and confirm GPIO15 blinks while the pairing window is open
5. **Capture IR Output**: Verify the emitted Trotec 3550 frames with an IR receiver or logic analyzer
6. **Test with Real AC**: Verify the AC responds to power, Off/Cool mode, and temperature commands

### Boot Pairing Mode Regression

```bash
# Normal boot
# Expected: GPIO15 stays off and no boot-requested pairing-window log appears.

# Boot while holding D1 / GPIO1 to GND, then release after startup logs begin
# Expected: GPIO15 blinks, the commissioning window opens for 300 seconds, and existing fabrics are not erased.

# After normal boot, hold D1 / GPIO1 to GND for 15 seconds
# Expected: GPIO15 fast-blinks five times, Matter commissioning data is reset, and the device reboots into commissioning.

# Stuck-button check
# Expected: if GPIO1 remains low for 30 seconds during boot pairing, startup continues and the commissioning window opens once.
```

## Success Criteria

✅ **Build completes without errors**  
✅ **Device boots and creates AC endpoint**  
✅ **Device can be commissioned via Matter**  
✅ **Power commands work (OnOff cluster)**  
✅ **Mode commands work (Thermostat cluster)**  
✅ **Temperature commands work (Thermostat cluster)**  
✅ **All debug logs appear correctly**  
✅ **Matter attributes can be read back**

## Performance Notes

- **Commissioning time**: ~30-60 seconds
- **Command response**: <2 seconds
- **Memory usage**: Check with `idf.py size` 
- **Power consumption**: Monitor during operation

This setup provides a Matter-compatible AC controller with Trotec 3550 IR transmission.
