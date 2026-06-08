# Sröme AC

Matter-compatible IR controller for the Sröme AC YPS-12C using Seeed Studio XIAO ESP32-C6 and ESP-Matter.

The device exposes a Matter Room Air Conditioner endpoint and transmits Trotec 3550-compatible IR state frames for power, mode, and cooling setpoint changes. Because the AC does not report state back over IR, the Matter AC state is optimistic. Local temperature is reported from a DS18B20 external temperature sensor on GPIO2, with a 20.0 C fallback until a valid sensor reading is available.

Note: This project is still a work in progress. Some parts of the code and documentation have been refactored with AI assistance.

## Home Assistant Matter Device

These screenshots show the added Matter device in Home Assistant. The detailed view exposes the measured local temperature, cooling setpoint, and Cool mode control.

![Sröme AC Home Assistant detail view](<docs/Screenshot 2026-06-08 202114.png>)

The area card shows the same Matter device as a compact room control.

![Sröme AC Home Assistant area card](<docs/Screenshot 2026-06-08 202146.png>)

## Hardware Wiring

The reference board is the [Seeed Studio XIAO ESP32-C6](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/). The XIAO pin map lists D2 as GPIO2 and D3 as GPIO21, which are used here for the DS18B20 data line and IR transmitter output.

```mermaid
flowchart LR
    subgraph XIAO["Seeed Studio XIAO ESP32-C6"]
        V3V3["3V3"]
        GND["GND"]
        D2["D2 / GPIO2"]
        D3["D3 / GPIO21"]
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

    V3V3 --> T_VDD
    GND --> T_GND
    D2 --> T_DQ
    V3V3 -->|"4.7 kOhm pull-up"| T_DQ

    D3 --> IR_IN
    V3V3 --> IR_PWR
    GND --> IR_GND
    IR_IN --> IR_LED

    FW["Firmware boot config"]
    FW -. "drives low" .-> RFEN
    FW -. "drives high for external antenna" .-> RFSEL
```

| XIAO pin | ESP32-C6 GPIO | Connected hardware | Notes |
|----------|---------------|--------------------|-------|
| D2 | GPIO2 | DS18B20 DQ | Add 4.7 kOhm pull-up to 3.3 V |
| D3 | GPIO21 | IR transmitter driver input | Manual 38 kHz carrier output |
| 3V3 | 3.3 V | DS18B20 VDD, pull-up, low-current logic supply | Use a proper IR LED driver for LED current |
| GND | GND | Common ground | Shared by ESP32-C6, DS18B20, and IR driver |
| RF switch power | GPIO3 | XIAO RF switch control | Driven low before selecting antenna |
| RF switch select | GPIO14 | XIAO RF switch select | Driven high for external antenna |

## Included Components

- [feelfreelinux/ds18b20](https://github.com/feelfreelinux/ds18b20): Original source for the DS18B20 1-Wire temperature sensor library used in `components/ds18b20`.
- [crankyoldgit/IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266): Reference implementation for the Trotec 3550 IR protocol used by the Sröme AC YPS-12C frame encoder.

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
