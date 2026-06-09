# Ströme AC Matter Controller

This project implements a Matter-compatible Room Air Conditioner controller for the Ströme AC YPS-12C using ESP32-Matter SDK. The controller exposes a conservative AC surface for power, Off/Cool mode, and cooling temperature, and sends Trotec 3550-compatible IR state frames to the AC unit.

## Features

### Main AC Control Functions
- **Power**: Matter OnOff control:
  - On: AC powered on in Cool mode
  - Off: AC powered off
- **Set Mode state**: Thermostat mode control:
  - Cool: Standard cooling mode
  - Off: AC powered off
- **Set Temperature**: Cooling setpoint from 16-30 C

### Matter Control Coverage

The Ströme AC IR protocol is state-based, so every Matter control writes a complete IR payload containing power, mode, temperature, fan speed, swing, Celsius unit, and checksum. Fan speed and swing are currently held at firmware defaults instead of being exposed over Matter.

| AC / IR Function | Matter Control | Firmware Status |
|------------------|----------------|-----------------|
| Power on/off | OnOff `OnOff`; mirrored with Thermostat `SystemMode` | Supported |
| Cooling mode | Thermostat `SystemMode=Cool`; also set by OnOff `true` | Supported |
| Off mode | Thermostat `SystemMode=Off`; also set by OnOff `false` | Supported |
| Temperature 16-30 C | Thermostat `OccupiedCoolingSetpoint`; `OccupiedHeatingSetpoint` is mirrored as a controller compatibility alias | Setpoint is stored and sent in every IR state frame |
| Fan speed Low/Medium/High | Not exposed over Matter | Firmware sends the stored default speed in every IR frame |
| Vertical swing | Not exposed over Matter | Firmware sends the stored default swing state in every IR frame |
| Current room temperature | Thermostat `LocalTemperature` and Temperature Measurement | DS18B20 external sensor on GPIO2; falls back to 20.0 C until a valid reading is available |
| Thermostat operating state | `PICoolingDemand`, `ThermostatRunningMode`, and `ThermostatRunningState` | Optimistic compatibility values derived from the last IR command |
| Fan-only / dry / heat / auto modes | Rejected if a controller writes them | Not exposed in the simplified thermostat model |
| Timer hours / timer enable | Not exposed | Intentionally disabled in emitted frames |
| Fahrenheit mode | Not exposed | Firmware always emits Celsius mode |

Note: Fan Control is intentionally not advertised in the first Room Air Conditioner phase. The minimal AC endpoint is verified first; fan speed will be added later with the Matter Fan Control `MultiSpeed` feature and `SpeedMax=3`.

### Technical Implementation

#### Matter Clusters Used
1. **OnOff Cluster**: Manages AC power and mirrors Off/Cool system mode
2. **Thermostat Cluster**: Manages Off/Cool mode and temperature setpoint
3. **Temperature Measurement Cluster**: DS18B20 local-temperature feedback, with 20.0 C fallback
4. **Thermostat UI Configuration Cluster**: Basic thermostat UI metadata

#### Device Configuration
- **Matter Product Name**: Ströme AC
- **Model**: YPS-12C
- **Device Type**: Room Air Conditioner
- **Power Model**: Mains-powered, always-on Matter node. ICD server support is disabled; a small root compatibility shim is present for RainMaker Matter reads.
- **Default Values**:
  - Power: ON
  - Temperature: 24.0°C
  - Fan Speed: Medium (IR default, not exposed over Matter)
  - AC Mode: Cool
  - Fan Swing: Off (IR default, not exposed over Matter)
  - Local Temperature: DS18B20 reading when valid, otherwise 20.0°C

## File Structure

### Core Files
- `main/app_main.cpp`: Main application logic and Matter node setup
- `main/app_driver.cpp`: AC control driver implementation and Matter attribute mapping
- `main/trotec_3550_ir.cpp`: Ströme AC YPS-12C/Trotec 3550-compatible IR frame encoder/transmitter using manual 38 kHz GPIO carrier generation
- `main/app_priv.h`: Private headers and AC state definitions
- `components/ds18b20/`: External DS18B20 1-Wire temperature sensor component

### Key Functions

#### AC Control Functions
```cpp
esp_err_t app_driver_ac_set_mode(ac_mode_t mode);           // Off/Cool mode selection
esp_err_t app_driver_ac_set_temperature(int16_t temp);      // Temperature
```

#### Matter Attribute Handlers
- `app_driver_attribute_update()`: Processes Matter attribute updates
- `app_driver_room_air_conditioner_set_defaults()`: Sets initial AC state

## Building and Running

### Prerequisites
1. ESP-IDF v5.4.1+ installed and configured
2. ESP-Matter SDK cloned and set up
3. ESP32 development board (esp32c6 in examples below)

### Build Commands
```bash
# Set up environment
cd esp-idf && source ./export.sh && cd ..
cd esp-matter && source ./export.sh && cd ..

# Configure and build
cd strome-ir-matter
idf.py set-target esp32c6
idf.py build

# Flash and monitor
idf.py flash monitor
```

### Commissioning
The firmware registers a project-specific Matter commissionable data provider before Matter starts. The setup PIN and discriminator are derived from the ESP32-C6 factory MAC address, so the onboarding payload is stable across flashes but unique to this physical board.

QR code, manual pairing code, setup PIN, and discriminator are printed by the device at boot from `PrintOnboardingCodes()`.

Use chip-tool or other Matter commissioner:
```bash
chip-tool pairing ble-thread <node-id> hex:<thread-dataset-hex> <setup-pin> <discriminator>
```

## Matter Control Commands

### Thermostat Control
```bash
# Power control through the Room Air Conditioner OnOff cluster
chip-tool onoff on <node-id> 1
chip-tool onoff off <node-id> 1

# Set mode / power (Matter SystemMode values)
chip-tool thermostat write system-mode 0 <node-id> 1  # Off
chip-tool thermostat write system-mode 3 <node-id> 1  # Cool

# Set stored cooling temperature (in 0.01°C units, accepted range: 1600-3000)
chip-tool thermostat write occupied-cooling-setpoint 2400 <node-id> 1  # 24.0°C
```

## IR Implementation

The IR implementation is state-based, matching the Trotec 3550-compatible protocol used by the Ströme AC YPS-12C. Every accepted Matter write sends a complete 72-bit AC state frame on GPIO 21 with a manually generated 38 kHz carrier. The frame includes power, mode, temperature, fan speed, swing, Celsius unit flag, and checksum.

RF switch control is configured at boot:
- GPIO3 is driven low first to enable RF switch control.
- GPIO14 is driven high to select the external antenna.

## Local Temperature Sensor

The Matter `LocalTemperature` and Temperature Measurement values come from a DS18B20 sensor:
- DS18B20 data pin: GPIO2
- Add a 4.7 kOhm pull-up resistor between data and 3.3 V.
- The firmware polls the sensor every 10 seconds.
- Until a plausible indoor reading is available, Matter reports the fallback value of 20.0 C.
- Readings outside 5.0-45.0 C are ignored so an unplugged sensor does not overwrite the fallback with the library's 0 C failure value.

## AC Mode Mapping

| Matter Thermostat SystemMode | AC Function | Description |
|------------------------------|-------------|-------------|
| 0 (Off)                      | Off         | AC completely off |
| 3 (Cool)                     | Cool        | Cooling mode with compressor |

## Testing

### Logs
Monitor ESP32 logs to see AC control commands:
```
I (12345) app_driver: Ströme AC Power: ON
I (12346) trotec_3550_ir: TX state: power=on mode=3 fan=2 swing=0 temp=24.00C frame=...
I (12347) app_driver: Ströme AC Mode: COOL
```

### Matter Compatibility
The implementation follows Matter specification for:
- Room Air Conditioner device type
- Standard OnOff cluster with DeadFront behavior from the generated SDK helper
- Standard Thermostat cluster with the Cooling feature
- Always-on behavior without ICD advertisement
- Validation of unsupported modes and setpoint range before IR control

## Future Enhancements

1. **Fan Speed**: Add Fan Control `MultiSpeed` with `SpeedMax=3`, mapped to IR Low/Medium/High
2. **Swing**: Add a controller-visible control once a suitable Matter representation is chosen
3. **Additional Modes**: Add documented dry and fan-only IR modes after the minimal AC endpoint is stable
4. **Temperature Feedback**: Tune DS18B20 placement/filtering after real-room testing
5. **Scheduling**: Add timer and scheduling functionality
