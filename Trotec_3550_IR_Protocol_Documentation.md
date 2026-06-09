# Ströme AC YPS-12C / Trotec 3550 IR Protocol Documentation

## Table of Contents
1. [Protocol Overview](#protocol-overview)
2. [Signal Characteristics](#signal-characteristics)
3. [Data Structure](#data-structure)
4. [Temperature Encoding](#temperature-encoding)
5. [Command Analysis](#command-analysis)
6. [Implementation Guide](#implementation-guide)
7. [Hardware Setup](#hardware-setup)
8. [Code Examples](#code-examples)
9. [Troubleshooting](#troubleshooting)

---

## Protocol Overview

The **Ströme AC YPS-12C** uses a Trotec 3550-compatible proprietary IR protocol that is part of the IRremoteESP8266 library. This protocol uses **state-based commands** where the entire AC state is transmitted in each command, rather than individual parameter changes.

Protocol reference: [crankyoldgit/IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266), especially its Trotec 3550 encoder/decoder implementation.

Payload and IR signal debugging were also performed with Arduino and [AnalysIR](https://github.com/AnalysIR) tooling to verify the control data, timing, and packet structure against captured remote-control frames.

Note: This protocol documentation is based on captured IR signals from the original remote and practical AC response testing. The payload fields and timings are not guaranteed to be a complete or official specification, and some unknown/reserved bits may still be interpreted only by observed behavior.

### Key Features:
- **Protocol Type**: PulseDistance encoding
- **Data Length**: 72 bits (9 bytes)
- **Bit Order**: LSB (Least Significant Bit) first
- **Carrier Frequency**: 38 kHz
- **State-based**: Complete system state transmitted with each command

### ESP32-C6 implementation note

The earlier verified Ströme YPS-12C ESP32-C6 test project used manual GPIO carrier generation and sent each byte MSB-first. Some IRremoteESP8266 raw dumps describe the aggregate decoded value as LSB-first, but the AC was validated with the MSB-first byte transmission path used by this firmware.

---

## Signal Characteristics

### Timing Parameters
```cpp
const uint16_t kTrotec3550HdrMark = 12000;    // Header mark: 12ms
const uint16_t kTrotec3550HdrSpace = 5130;    // Header space: 5.13ms
const uint16_t kTrotec3550BitMark = 550;      // Bit mark: 550µs
const uint16_t kTrotec3550OneSpace = 1950;    // '1' space: 1.95ms
const uint16_t kTrotec3550ZeroSpace = 500;    // '0' space: 500µs
```

### Signal Structure
```
[HEADER] [72 DATA BITS] [STOP BIT]
  12ms     9 bytes       550µs
  5.13ms   LSB first     gap
```

### Example Raw Signal (Power ON)
```cpp
Protocol=PulseDistance Raw-Data=0xBA 72 bits LSB first
Gap=3276750us Duration=120200us
Send parameters: 38kHz, 12000, 5000, 550, 1950, 550, 550
Data: {0xB00046AA, 0x11880000, 0xBA}
```

---

## Data Structure

### 72-bit Frame Layout (9 bytes)
```
Byte 0: [7:0] - Intro/Header (Fixed: 0x55)
Byte 1: [7:0] - Power, Swing, Timer, Temperature (°C)
Byte 2: [7:0] - Timer Hours, Unknown bits
Byte 3: [7:0] - Temperature (°F), Unknown bits  
Byte 4: [7:0] - Unknown/Reserved
Byte 5: [7:0] - Unknown/Reserved
Byte 6: [7:0] - Mode, Fan Speed, Unknown bits
Byte 7: [7:0] - Temperature Unit, Unknown bits
Byte 8: [7:0] - Checksum
```

### Byte-level Breakdown
```cpp
union Trotec3550Protocol {
  uint8_t raw[9];
  struct {
    // Byte 0
    uint8_t Intro:    8;  // Fixed value (0x55)
    // Byte 1  
    uint8_t SwingV   :1;  // Vertical swing
    uint8_t Power    :1;  // Power state
    uint8_t          :1;  // Unknown
    uint8_t TimerSet :1;  // Timer enabled
    uint8_t TempC    :4;  // Temperature (°C offset)
    // Byte 2
    uint8_t TimerHrs :4;  // Timer hours
    uint8_t          :4;  // Unknown
    // Byte 3
    uint8_t TempF    :5;  // Temperature (°F offset)
    uint8_t          :3;  // Unknown
    // Byte 6
    uint8_t Mode     :2;  // Operating mode
    uint8_t          :2;  // Unknown
    uint8_t Fan      :2;  // Fan speed
    uint8_t          :2;  // Unknown
    // Byte 7
    uint8_t          :7;  // Unknown
    uint8_t Celsius  :1;  // Temperature unit
    // Byte 8
    uint8_t Sum      :8;  // Checksum
  };
};
```

---

## Temperature Encoding

### Temperature Ranges
```cpp
const uint8_t kTrotec3550MinTempC = 16;  // Minimum: 16°C
const uint8_t kTrotec3550MaxTempC = 30;  // Maximum: 30°C
const uint8_t kTrotec3550MinTempF = 59;  // Minimum: 59°F
const uint8_t kTrotec3550MaxTempF = 86;  // Maximum: 86°F
```

### Encoding Method
The protocol uses **dual temperature encoding**:
1. **Celsius encoding** in Byte 1 (4 bits): `TempC = actual_temp - 16`
2. **Fahrenheit encoding** in Byte 3 (5 bits): `TempF = actual_temp_f - 59`

### Temperature Lookup Table
| Celsius | TempC Bits | TempF Bits | Example Command |
|---------|------------|------------|-----------------|
| 20°C    | 0x4 (4)    | 0x9 (9)    | 0x900042AA, 0x1880000, 0x8C |
| 22°C    | 0x6 (6)    | 0xD (13)   | 0xB00046AA, 0x18C0000, 0xAE |
| 24°C    | 0x8 (8)    | 0x11 (17)  | 0x80041AA, 0x18C0000, 0x19 |
| 26°C    | 0xA (10)   | 0x15 (21)  | 0x280045AA, 0x1880000, 0x39 |
| 30°C    | 0xE (14)   | 0x1B (27)  | 0xD80047AA, 0x1880000, 0xC7 |

---

## Command Analysis

### Power Commands
```cpp
// Power ON
{0xB00046AA, 0x11880000, 0xBA}
// Analysis: Power=1, Mode=Cool, Fan=High, Temp=22°C

// Power OFF  
{0xB00006AA, 0x11880000, 0xDA}
// Analysis: Power=0, other parameters maintained
```

### Fan Speed Commands
```cpp
// High Fan
{0xB00046AA, 0x18C0000, 0xAE}  // Fan bits: 11 (3)

// Medium Fan
{0xB00046AA, 0x1840000, 0xA6}  // Fan bits: 10 (2)

// Low Fan
{0xB00046AA, 0x1880000, 0xAA}  // Fan bits: 01 (1)
```

### Mode Commands
```cpp
const uint8_t kTrotecAuto = 0;  // Auto mode
const uint8_t kTrotecCool = 1;  // Cooling mode
const uint8_t kTrotecDry = 2;   // Dry/Dehumidify mode
const uint8_t kTrotecFan = 3;   // Fan only mode
```

### Feature Commands
```cpp
// Toggle Swing
{0xB000C6AA, 0x1840000, 0x66}

// Toggle Mode
{0xB000C6AA, 0x1C40000, 0x16}
```

---

## Implementation Guide

### Method 1: Using IRremoteESP8266 Library

#### Include Required Libraries
```cpp
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Trotec.h>
```

#### Basic Setup
```cpp
const uint16_t kIrLed = 21;  // GPIO pin for IR LED
IRTrotec3550 ac(kIrLed);   // Create Trotec 3550 object

void setup() {
  ac.begin();              // Initialize IR transmitter
  Serial.begin(115200);
}
```

#### Sending Commands
```cpp
void sendACCommand() {
  ac.setPower(true);           // Turn on
  ac.setMode(kTrotecCool);     // Set cooling mode (REQUIRED)
  ac.setTemp(24, true);        // Set 24°C (true = Celsius)
  ac.setFan(3);                // High fan (1=Low, 2=Med, 3=High)
  ac.setSwingV(false);         // Disable vertical swing
  ac.send();                   // Transmit command
}
```

### Method 2: Using Raw Data

#### Raw Data Transmission
```cpp
IRsend irsend(kIrLed);

void sendRawCommand() {
  // Power ON command raw data
  uint32_t powerOnData[] = {0xB00046AA, 0x11880000, 0xBA};
  
  irsend.sendPulseDistanceWidthFromArray(
    38,           // 38kHz carrier
    12000,        // Header mark
    5000,         // Header space  
    550,          // Bit mark
    1950,         // '1' space
    550,          // Bit mark
    550,          // '0' space
    powerOnData,  // Data array
    72,           // Bits to send
    PROTOCOL_IS_LSB_FIRST,  // LSB first
    0,            // Repeat period
    0             // Number of repeats
  );
}
```

### Method 3: Using Generic IRac Interface
```cpp
#include <IRac.h>

IRac ac(kIrLed);

void setup() {
  ac.next.protocol = decode_type_t::TROTEC_3550;
  ac.next.model = 1;
  ac.next.power = true;
  ac.next.mode = stdAc::opmode_t::kCool;
  ac.next.degrees = 24;
  ac.next.celsius = true;
  ac.next.fanspeed = stdAc::fanspeed_t::kMedium;
  ac.sendAc();
}
```

---

## Hardware Setup

### IR LED Circuit
```
ESP32/ESP8266 GPIO --> 2N2222 Transistor --> IR LED --> GND
                   |
                   --> 10kΩ Resistor --> 3.3V
```

### Component Requirements
- **IR LED**: 940nm wavelength (common types: TSAL6200, TSAL6400)
- **Transistor**: 2N2222 or similar NPN transistor
- **Resistor**: 10kΩ pull-up resistor
- **Current limiting resistor**: 220Ω for IR LED

### Wiring Diagram
```
ESP32 GPIO21 ----[10kΩ]---- 3.3V
     |
     |----[Base] 2N2222 [Collector]----[220Ω]----[IR LED Anode]
                         [Emitter]                [IR LED Cathode]
                             |                         |
                            GND ----------------------GND
```

### GPIO Pin Selection
- **Recommended**: GPIO 21
- **Avoid**: GPIO 0, 1, 3 (boot/serial pins)
- **Alternative**: GPIO 14, 12, 13, 15

---

## Code Examples

### Complete Working Example
```cpp
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Trotec.h>

const uint16_t kIrLed = 21;
IRTrotec3550 ac(kIrLed);

void setup() {
  ac.begin();
  Serial.begin(115200);
  Serial.println("Trotec 3550 IR Controller Ready");
}

void loop() {
  // Turn ON with cooling mode
  Serial.println("Turning ON AC...");
  ac.setPower(true);
  ac.setMode(kTrotecCool);     // ESSENTIAL: Mode must be set
  ac.setTemp(22, true);        // 22°C
  ac.setFan(3);                // High fan
  ac.send();
  delay(10000);
  
  // Change temperature
  Serial.println("Changing to 26°C...");
  ac.setTemp(26, true);
  ac.setFan(1);                // Low fan
  ac.send();
  delay(10000);
  
  // Turn OFF
  Serial.println("Turning OFF AC...");
  ac.setPower(false);
  ac.send();
  delay(10000);
}
```

### Debug Version with State Monitoring
```cpp
void debugACState() {
  uint8_t* state = ac.getRaw();
  Serial.print("AC State: ");
  for(int i = 0; i < 9; i++) {
    Serial.print("0x");
    if(state[i] < 0x10) Serial.print("0");
    Serial.print(state[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  
  Serial.println("Settings:");
  Serial.print("Power: "); Serial.println(ac.getPower() ? "ON" : "OFF");
  Serial.print("Mode: "); Serial.println(ac.getMode());
  Serial.print("Temp: "); Serial.print(ac.getTemp()); Serial.println("°C");
  Serial.print("Fan: "); Serial.println(ac.getFan());
  Serial.print("Swing: "); Serial.println(ac.getSwingV() ? "ON" : "OFF");
}
```

### Temperature Sweep Example
```cpp
void temperatureSweep() {
  for(int temp = 20; temp <= 30; temp += 2) {
    Serial.print("Setting temperature to: ");
    Serial.print(temp);
    Serial.println("°C");
    
    ac.setPower(true);
    ac.setMode(kTrotecCool);
    ac.setTemp(temp, true);
    ac.setFan(2);  // Medium fan
    ac.send();
    
    delay(5000);  // Wait 5 seconds
  }
}
```

---

## Troubleshooting

### Common Issues and Solutions

#### 1. AC Not Responding
**Problem**: Commands sent but AC doesn't respond
**Solutions**:
- ✅ Ensure `setMode()` is called before `send()`
- ✅ Check IR LED circuit (use phone camera to verify)
- ✅ Verify correct GPIO pin wiring
- ✅ Test from closer distance (1-2 meters)
- ✅ Check AC is in IR receiver mode

#### 2. Compilation Errors
**Problem**: Undefined constants or methods
**Solutions**:
```cpp
// Define missing constants manually
const uint8_t kTrotec3550FanLow = 1;
const uint8_t kTrotec3550FanMed = 2;
const uint8_t kTrotec3550FanHigh = 3;

// Ensure correct includes
#include <ir_Trotec.h>  // Must include for Trotec protocols
```

#### 3. Inconsistent Behavior
**Problem**: Commands work sometimes
**Solutions**:
- Add delays between commands (minimum 1000ms)
- Reset AC state before new commands
- Use debug output to verify transmitted data
- Check for IR interference from other devices

#### 4. Library vs Raw Data Mismatch
**Problem**: Library generates different data than captured
**Debug Method**:
```cpp
// Compare library output with known working data
uint8_t* libraryState = ac.getRaw();
uint8_t knownWorking[] = {0xAA, 0x60, 0x0D, 0x00, 0x00, 0x10, 0x88, 0x5A};

Serial.println("Library vs Known Data:");
for(int i = 0; i < 9; i++) {
  Serial.print("Byte "); Serial.print(i); Serial.print(": ");
  Serial.print("Lib=0x"); Serial.print(libraryState[i], HEX);
  Serial.print(" Known=0x"); Serial.println(knownWorking[i], HEX);
}
```

### Hardware Debugging

#### IR LED Test
```cpp
// Simple IR LED test - should be visible through phone camera
void testIRLED() {
  for(int i = 0; i < 10; i++) {
    digitalWrite(kIrLed, HIGH);
    delayMicroseconds(26);  // 38kHz half period
    digitalWrite(kIrLed, LOW);
    delayMicroseconds(26);
  }
  delay(1000);
}
```

#### Signal Timing Verification
Use oscilloscope or logic analyzer to verify:
- Header timing: 12ms mark, 5.13ms space
- Bit timing: 550µs marks, 1950µs/'1' or 500µs/'0' spaces
- Overall frame duration: ~120ms

### Protocol Validation

#### Checksum Verification
```cpp
bool validateChecksum(uint8_t* data) {
  uint8_t calculated = 0;
  for(int i = 0; i < 8; i++) {
    calculated += data[i];
  }
  return (calculated == data[8]);
}
```

---

## Conclusion

The Trotec 3550 IR protocol is a sophisticated **state-based protocol** that requires careful attention to:

1. **Complete state specification** (power + mode + temperature + fan)
2. **Proper timing parameters** (38kHz, specific mark/space durations)
3. **Correct hardware setup** (transistor-driven IR LED circuit)
4. **Mode setting requirement** (essential for power-on commands)

This documentation provides the foundation for reliable Trotec 3550 AC control using ESP32/ESP8266 microcontrollers.

---

## References

- [IRremoteESP8266 Library](https://github.com/crankyoldgit/IRremoteESP8266)
- [IR Protocol Analysis Tools](https://github.com/crankyoldgit/IRremoteESP8266/wiki)
- [ESP32/ESP8266 IR Sending Circuit](https://github.com/crankyoldgit/IRremoteESP8266/wiki#ir-sending)

*Document Version: 1.0*  
*Last Updated: June 8, 2026*
