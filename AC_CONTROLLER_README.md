# Ströme AC Matter Controller

This project implements a Matter-compatible Room Air Conditioner controller for the Ströme AC YPS-12C using ESP32-Matter SDK. The controller exposes a conservative AC surface for power, Cool/Fan/Dry mode, fan speed, and cooling temperature, and sends Trotec 3550-compatible IR state frames to the AC unit.

## Features

### Main AC Control Functions
- **Power**: Matter OnOff control:
  - On: AC powered on in the last selected active mode
  - Off: AC powered off
- **Set Mode state**: Thermostat mode control:
  - Cool: Standard cooling mode
  - FanOnly: Fan-only mode
  - Dry: Dehumidify mode
- **Set Temperature**: Cooling setpoint from 16-30 C

### Matter Control Coverage

The Ströme AC IR protocol is state-based, so every Matter control writes a complete IR payload containing power, mode, temperature, fan speed, swing, Celsius unit, and checksum. Fan speed is exposed through Matter Fan Control with the MultiSpeed feature, and vertical swing is exposed through the Fan Control Rocking feature.

| AC / IR Function | Matter Control | Firmware Status |
|------------------|----------------|-----------------|
| Power on/off | OnOff `OnOff` | Supported |
| Cooling mode | Thermostat `SystemMode=Cool` | Supported |
| Fan-only mode | Thermostat `SystemMode=Off` | Home Assistant compatibility mapping |
| Dry / dehumidify mode | Thermostat `SystemMode=Heat` | Home Assistant compatibility mapping; the physical AC does not heat |
| Temperature 16-30 C | Thermostat `OccupiedCoolingSetpoint`; `OccupiedHeatingSetpoint` is mirrored as a controller compatibility alias | Setpoint is stored and sent in every IR state frame |
| Fan speed Low/Medium/High | Fan Control `FanMode`, `SpeedSetting`, and `PercentSetting` | Stored speed is sent in every IR frame |
| Vertical swing | Fan Control `RockSetting` with `RockUpDown` | Stored swing state is sent in every IR frame |
| Current room temperature | Thermostat `LocalTemperature` and Temperature Measurement | DS18B20 external sensor on GPIO2; falls back to 20.0 C until a valid reading is available |
| Thermostat operating state | `PICoolingDemand`, `ThermostatRunningMode`, and `ThermostatRunningState` | Optimistic compatibility values derived from the last IR command |
| Auto / emergency heat modes through Thermostat SystemMode | Rejected if a controller writes them | Unsupported by the physical AC |
| Timer hours / timer enable | Not exposed | Intentionally disabled in emitted frames |
| Fahrenheit mode | Not exposed | Firmware always emits Celsius mode |

Note: Fan Control advertises Matter `MultiSpeed` with `SpeedMax=3`. Some controllers may show both percentage and discrete speed controls because both are standard Fan Control attributes. Some controllers may also render the Fan Control cluster as a separate fan entity even though it is on the same Room Air Conditioner endpoint. The standard Fan Control model includes an Off state; for this AC, Fan Control Off, `SpeedSetting=0`, and `PercentSetting=0` are treated as AC power-off aliases and mapped to OnOff `false`.

Note: Vertical swing is exposed through the Matter Fan Control **Rocking** feature. The AC supports only `RockUpDown`, so `RockSupport=0x02`, `RockSetting=0x00` means swing off, and `RockSetting=0x02` means vertical swing on. Unsupported rocking bits are rejected and do not send IR.

Note: Thermostat `ControlSequenceOfOperation` and the heating feature are enabled only as a Home Assistant UI compatibility workaround. Thermostat `SystemMode=Heat` is remapped to AC dry/dehumidify mode, not heating. Thermostat `SystemMode=Off` is remapped to fan-only mode. Actual AC power off is controlled through the OnOff cluster.

### System Mode Mapping

The Matter Thermostat cluster uses standard labels that do not exactly match the Ströme AC remote. The firmware deliberately maps those labels to the three native Ströme modes:

| Controller label | Matter `SystemMode` | Ströme AC mode | Actual behavior |
|------------------|---------------------|----------------|-----------------|
| Off | `0` | Fan | Sends a full IR frame with power on and mode set to fan-only. |
| Cool | `3` | Cool | Sends a full IR frame with power on and mode set to cooling. |
| Heat | `4` | Dry | Sends a full IR frame with power on and mode set to dry/dehumidify. The AC does not heat. |

To turn the AC completely off, use the Power / OnOff control. Do not use Thermostat `SystemMode=Off` for power-off in this project.

Implementation note: OnOff, Thermostat SystemMode, and Fan Control are not independent firmware states. All three control surfaces update the same stored AC state and then emit one full IR frame. This keeps the protocol-required OnOff cluster while allowing mode and fan writes to behave like power controls when they imply on/off.

### Matter And Home Assistant Limitations

Matter models this AC through multiple standard clusters. Home Assistant and other controllers may split those clusters into multiple UI entities even though the firmware keeps one internal AC state.

- The Room Air Conditioner endpoint can appear as separate climate, fan, mode, power, and sensor entities in Home Assistant.
- The Home Assistant climate mode picker uses compatibility labels: Off = Fan, Heat = Dry, Cool = Cool. This is not strict Matter thermostat semantics, but it keeps all native AC modes reachable from the climate card.
- The Fan Control cluster is standard Matter behavior, so controllers may show both percentage and discrete speed controls.
- Fan Control Off, `SpeedSetting=0`, and `PercentSetting=0` all mean whole-device power off for this AC.
- State is optimistic because the AC does not send IR status back to the controller.

### Technical Implementation

#### Matter Clusters Used
1. **OnOff Cluster**: Manages AC power
2. **Thermostat Cluster**: Manages Cool/Fan/Dry mode and temperature setpoint
3. **Fan Control Cluster**: Manages Low/Medium/High fan speed through `FanMode`, `SpeedSetting`, and `PercentSetting`; manages vertical swing through Rocking `RockSupport=0x02` and `RockSetting`
4. **Temperature Measurement Cluster**: DS18B20 local-temperature feedback, with 20.0 C fallback
5. **Thermostat UI Configuration Cluster**: Basic thermostat UI metadata

#### Device Configuration
- **Matter Product Name**: Ströme AC
- **Model**: YPS-12C
- **Device Type**: Room Air Conditioner
- **Minimum Matter Version Target**: Matter 1.2, because this implementation uses the official Room Air Conditioner device type (`0x0072`)
- **Power Model**: Mains-powered, always-on Matter node. ICD server support is disabled; a small root compatibility shim is present for RainMaker Matter reads.
- **Default Values**:
  - Power: ON
  - Temperature: 22.0°C
  - Fan Speed: Medium
  - AC Mode: Cool
  - Fan Swing: Off
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
esp_err_t app_driver_ac_set_mode(ac_mode_t mode);           // Cool/Fan/Dry mode selection
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

After changing the Matter data model, such as adding or removing Fan Control or other clusters, remove the old device from the Matter controller and recommission it. Many controllers cache endpoint and cluster layouts, so flashing alone may not make new controls appear.

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

# Set active mode through the Home Assistant thermostat compatibility mapping
chip-tool thermostat write system-mode 0 <node-id> 1  # Fan-only
chip-tool thermostat write system-mode 3 <node-id> 1  # Cool
chip-tool thermostat write system-mode 4 <node-id> 1  # Dry / dehumidify, shown as Heat by some controllers

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

These are the same values used by the Home Assistant climate mode selector:

| Matter Thermostat SystemMode | AC Function | Description |
|------------------------------|-------------|-------------|
| 0 (Off)                      | Fan         | Home Assistant compatibility mapping; does not power off the AC |
| 3 (Cool)                     | Cool        | Cooling mode with compressor |
| 4 (Heat)                     | Dry         | Home Assistant compatibility mapping; the physical AC does not heat |

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
- Standard Fan Control cluster with MultiSpeed `SpeedMax=3` and Rocking `RockUpDown`
- Always-on behavior without ICD advertisement
- Validation of unsupported modes and setpoint range before IR control

## Future Enhancements

1. **Temperature Feedback**: Tune DS18B20 placement/filtering after real-room testing
2. **Scheduling**: Add timer and scheduling functionality
