# Ströme AC

Matter-compatible IR controller for the Ströme AC YPS-12C using Seeed Studio XIAO ESP32-C6 and ESP-Matter.

The device exposes a Matter Room Air Conditioner endpoint and transmits Trotec 3550-compatible IR state frames for power, mode, fan speed, swing, and cooling setpoint changes. Because the AC does not report state back over IR, the Matter AC state is optimistic. Local temperature is reported from a DS18B20 external temperature sensor on GPIO2, with a 20.0 C fallback until a valid sensor reading is available.

Note: This project is still a work in progress. Some parts of the code and documentation have been refactored with AI assistance.

## TODO

- Timer control

## Matter Control Notes

Minimum Matter protocol target: **Matter 1.2**. The implementation uses the official Room Air Conditioner device type (`0x0072`), plus Thermostat, OnOff, Fan Control, and Temperature Measurement clusters. Controllers with Matter 1.2 or newer support are the intended target; older Matter 1.0/1.1 controllers may not recognize the Room Air Conditioner device type correctly.

Power-off is handled through the Room Air Conditioner `OnOff` cluster. Thermostat `SystemMode=Off` is not used for power-off in the Home Assistant compatibility mapping.

For Home Assistant climate-mode usability, Thermostat `SystemMode` is intentionally remapped: `Cool` means AC cooling, `Off` means AC fan-only, and `Heat` means AC dry/dehumidify. The Thermostat heating feature is advertised only to make this UI mapping possible; the physical AC still has no heating support.

### System Mode Mapping

Home Assistant and many Matter controllers label Thermostat `SystemMode` values using generic thermostat names. In this firmware those labels are intentionally reused as Ströme AC mode controls:

| Controller label | Matter `SystemMode` | Ströme AC mode | Actual behavior |
|------------------|---------------------|----------------|-----------------|
| Off | `0` | Fan | Turns the AC on in fan-only mode. This does **not** power off the device. |
| Cool | `3` | Cool | Turns the AC on in cooling mode and uses the configured temperature setpoint. |
| Heat | `4` | Dry | Turns the AC on in dry/dehumidify mode. The physical AC does **not** heat. |

Actual whole-device power off is always handled through the separate Power / OnOff control, or through Fan Control off/zero values.

The Matter `FanControl` cluster still includes a standard Off state. The physical AC cannot run cooling with its fan disabled, so Fan Control Off, `SpeedSetting=0`, and `PercentSetting=0` are treated as AC power-off aliases and mapped to `OnOff=false`.

### Rocking / Swing Mapping

Matter represents oscillating fan movement through the Fan Control **Rocking** feature. For this AC, that maps to the remote's vertical swing flag:

| Matter Fan Control attribute | Supported value | Ströme AC behavior |
|------------------------------|-----------------|--------------------|
| `RockSupport` | `0x02` / `RockUpDown` | Advertises vertical swing support only. |
| `RockSetting=0x00` | Off | Sends the next IR frame with vertical swing disabled. |
| `RockSetting=0x02` | Up/down rocking | Sends the next IR frame with vertical swing enabled. |

Other rocking directions, such as round or left/right rocking, are not supported by the documented IR payload and are rejected by the firmware.

Internally there is one AC state machine. OnOff, Thermostat SystemMode, and Fan Control writes all converge into the same stored power/mode/fan state before one complete IR frame is sent. Some controllers, including Home Assistant, may render these clusters as separate entities even though they belong to the same Room Air Conditioner endpoint.

## Matter And Controller Limitations

Matter exposes AC behavior through separate standard clusters rather than one app-specific control surface. A controller decides how to render those clusters, and that UI may not match the firmware's internal state model exactly.

Known limitations and controller behaviors:

- Home Assistant may render the same Room Air Conditioner endpoint as separate climate, fan, power, and sensor entities because `Thermostat`, `FanControl`, `OnOff`, and `TemperatureMeasurement` are separate Matter clusters.
- Home Assistant's climate mode picker is mapped for usability rather than strict semantic correctness: Off = Fan, Heat = Dry, Cool = Cool. The separate Power entity is the actual AC off/on control.
- The AC has no IR feedback path. If the original remote is used, or if an IR frame is missed, Matter state can drift from the real AC until the next command is sent.
- Fan Control includes standard off/zero values. This AC cannot cool with fan disabled, so those values are interpreted as whole-device power off.
- Fan speed and swing may appear as a separate fan entity in Home Assistant. That is a controller presentation choice; in firmware they still update the same AC state and emit one full IR frame.
- Timer support is intentionally not exposed yet.

## Matter Cluster Structure

The firmware exposes one main Room Air Conditioner endpoint. The AC is controlled optimistically: every accepted Matter write updates the stored state and sends one complete Trotec 3550-compatible IR frame.

| Endpoint | Device type | Server clusters | Purpose |
|----------|-------------|-----------------|---------|
| 0 | Root Node | Descriptor, Basic Information, Access Control, General Commissioning, Network Commissioning, Operational Credentials, compatibility ICD Management shim | Standard Matter node management, commissioning, product metadata, and networking |
| 1 | Room Air Conditioner (`0x0072`) | Descriptor, Identify, OnOff, Thermostat, Fan Control, Temperature Measurement, Thermostat UI Configuration | User-facing AC controls and local temperature reporting |

Endpoint 1 cluster behavior:

| Cluster | ID | Main attributes used | Firmware mapping |
|---------|----|----------------------|------------------|
| OnOff | `0x0006` | `OnOff` | Main power control. `false` sends an IR frame with power off; `true` powers on using the last selected active mode. |
| Thermostat | `0x0201` | `SystemMode`, `OccupiedCoolingSetpoint`, `LocalTemperature`, running-state attributes | Home Assistant compatibility mapping: Off = Fan, Heat = Dry, Cool = Cool. Setpoint writes update the stored cooling temperature. Local temperature is updated from the DS18B20 sensor. |
| Fan Control | `0x0202` | `FanMode`, `PercentSetting`, `PercentCurrent`, `SpeedMax=3`, `SpeedSetting`, `SpeedCurrent`, `RockSupport=0x02`, `RockSetting` | MultiSpeed fan control for Low, Medium, and High. Rocking `RockUpDown` maps to vertical swing. Zero/off values are treated as AC power-off aliases. |
| Temperature Measurement | `0x0402` | `MeasuredValue`, min/max measured values | Mirrors the DS18B20 local room temperature for controllers that prefer a sensor cluster. |
| Thermostat UI Configuration | `0x0204` | UI metadata | Basic thermostat display metadata for controller compatibility. |
| Identify | `0x0003` | Identify state | Standard Matter identify support. |

## Home Assistant Matter Device

These screenshots show the added Matter device in Home Assistant. Home Assistant renders the single Matter Room Air Conditioner endpoint as several UI surfaces because the endpoint exposes multiple standard clusters:

- **AC / climate control** from the Thermostat cluster.
- **Power** from the OnOff cluster.
- **Fan speed and oscillation** from the Fan Control cluster.

The device overview shows those controls grouped under one Matter device. `Power` is the actual on/off control, `YPS-12C` is the climate entity, and the fan entity controls speed plus oscillation/rocking.

![Home Assistant Matter device overview](<docs/Screenshot 2026-06-10 204950.png>)

The dashboard view shows the same split: climate and fan cards in the climate area, with the OnOff power control rendered separately.

![Home Assistant dashboard cards](<docs/Screenshot 2026-06-10 205022.png>)

The climate detail view exposes current temperature, cooling setpoint, and the remapped system modes. In this firmware, `Cool` means cooling, `Off` means fan-only, and `Heat` means dry/dehumidify.

![Home Assistant climate detail with mode menu](<docs/Screenshot 2026-06-10 205038.png>)

The fan detail view exposes Matter Fan Control. Low/Medium/High map to native fan speeds 1/2/3. The `Oscillating` control is Matter Rocking: `No` maps to `RockSetting=0x00`, and `Yes` maps to `RockSetting=0x02` / vertical swing.

![Home Assistant fan control and oscillation](<docs/Screenshot 2026-06-10 205057.png>)

The power detail view is the OnOff cluster. Use this to turn the AC fully off or back on.

![Home Assistant power control](<docs/Screenshot 2026-06-10 205123.png>)

## Hardware Wiring

The reference board is the [Seeed Studio XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/). The XIAO pin map lists D1 as GPIO1, D2 as GPIO2, D3 as GPIO21, and GPIO15 as the user LED. This project uses them for boot pairing, the DS18B20 data line, IR transmitter output, and the pairing indicator.

```mermaid
flowchart LR
    subgraph XIAO["Seeed Studio XIAO ESP32-C6"]
        V3V3["3V3"]
        GND["GND"]
        D1["D1 / GPIO1"]
        D2["D2 / GPIO2"]
        D3["D3 / GPIO21"]
        LED["GPIO15 / onboard LED"]
        RFEN["GPIO3 / RF switch power"]
        RFSEL["GPIO14 / RF switch select"]
    end

    subgraph TEMP["DS18B20 Temperature Sensor"]
        T_VDD["VDD"]
        T_DQ["DQ"]
        T_GND["GND"]
    end

    subgraph IR["IR Transmitter Driver"]
        IR_IN["Signal input"]
        IR_PWR["LED supply"]
        IR_GND["GND"]
        IR_LED["940 nm IR LED"]
    end

    subgraph PAIR["Pairing / Reset Button"]
        P_BTN["Momentary button"]
    end

    V3V3 --> T_VDD
    GND --> T_GND
    D2 --> T_DQ
    V3V3 -->|"4.7 kOhm pull-up"| T_DQ

    D3 --> IR_IN
    V3V3 --> IR_PWR
    GND --> IR_GND
    IR_IN --> IR_LED

    D1 --> P_BTN
    P_BTN --> GND

    FW["Firmware boot config"]
    FW -. "drives low" .-> RFEN
    FW -. "drives high for external antenna" .-> RFSEL
    FW -. "blinks during pairing mode" .-> LED
```

| XIAO pin | ESP32-C6 GPIO | Connected hardware | Notes |
|----------|---------------|--------------------|-------|
| D1 | GPIO1 | Pairing / reset button to GND | Active-low, internal pull-up enabled; no external pull-up required |
| D2 | GPIO2 | DS18B20 DQ | Add 4.7 kOhm pull-up to 3.3 V |
| D3 | GPIO21 | IR transmitter driver input | Manual 38 kHz carrier output |
| Onboard user LED | GPIO15 | Pairing mode indicator | Blinks while boot-requested Matter commissioning window is open |
| 3V3 | 3.3 V | DS18B20 VDD, pull-up, low-current logic supply | Use a proper IR LED driver for LED current |
| GND | GND | Common ground | Shared by ESP32-C6, DS18B20, and IR driver |
| RF switch power | GPIO3 | XIAO RF switch control | Driven low before selecting antenna |
| RF switch select | GPIO14 | XIAO RF switch select | Driven high for external antenna |

## Pairing / Reset Button

Add a normally-open momentary button between D1 / GPIO1 and GND. The firmware enables the ESP32-C6 internal pull-up, so the pin is normally high and becomes active when the button pulls it low.

After firmware changes that alter the Matter data model, remove the old device from the Matter controller and recommission it. Controllers often cache endpoint and cluster layouts, so newly added or removed clusters may not appear correctly after flashing alone.

There are two supported gestures:

| Gesture | Result | Existing Matter fabrics |
|---------|--------|-------------------------|
| Hold GPIO1 low during boot, then release after startup begins | Opens a Matter basic commissioning window for 300 seconds | Preserved |
| Hold GPIO1 low for 15 seconds after normal boot | Factory-resets Matter commissioning data, fast-blinks GPIO15 five times, then reboots | Removed |

Boot pairing is non-blocking: startup continues while the button is held. After Matter starts, the firmware waits for GPIO1 to be released before opening the commissioning window. If the button stays held for 30 seconds, the firmware opens the commissioning window once anyway so a stuck button does not permanently block pairing.

GPIO15, the onboard user LED, blinks while the boot-requested commissioning window is open. It turns off when commissioning completes, fails, or the window closes. The reset gesture uses five fast LED blinks before the reset flow continues.

## Included Components

- [feelfreelinux/ds18b20](https://github.com/feelfreelinux/ds18b20): Original source for the DS18B20 1-Wire temperature sensor library used in `components/ds18b20`.
- [crankyoldgit/IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266): Reference implementation for the Trotec 3550 IR protocol used by the Ströme AC YPS-12C frame encoder.

## Docs

- [AC controller overview](AC_CONTROLLER_README.md)
- [Trotec 3550 IR implementation](IR_IMPLEMENTATION.md)
- [Build and test guide](BUILD_AND_TEST.md)
- [Protocol documentation](Trotec_3550_IR_Protocol_Documentation.md)

## Build

```bash
idf.py set-target esp32c6
idf.py build
```
