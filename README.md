# Ströme AC

Matter-compatible IR controller for the Ströme AC YPS-12C using Seeed Studio XIAO ESP32-C6 and ESP-Matter.

The device exposes a Matter Room Air Conditioner endpoint and transmits Trotec 3550-compatible IR state frames for power, mode, and cooling setpoint changes. Because the AC does not report state back over IR, the Matter AC state is optimistic. Local temperature is reported from a DS18B20 external temperature sensor on GPIO2, with a 20.0 C fallback until a valid sensor reading is available.

Note: This project is still a work in progress. Some parts of the code and documentation have been refactored with AI assistance.

## TODO

- Swing control
- Timer control
- Fan speed control
- Addition of Dry and Fan modes

## Home Assistant Matter Device

These screenshots show the added Matter device in Home Assistant. The detailed view exposes the measured local temperature, cooling setpoint, and Cool mode control.

![Ströme AC Home Assistant detail view](<docs/Screenshot 2026-06-08 202114.png>)

The area card shows the same Matter device as a compact room control.

![Ströme AC Home Assistant area card](<docs/Screenshot 2026-06-08 202146.png>)

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
