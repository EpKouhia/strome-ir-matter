# Ströme AC YPS-12C IR Implementation

This project implements Ströme AC YPS-12C IR control using the Trotec 3550-compatible protocol.
It uses manual GPIO carrier generation on ESP32-C6 because this was validated against the actual AC unit and proved more reliable than RMT for this protocol.
Payload and IR signal debugging were performed with Arduino and [AnalysIR](https://github.com/AnalysIR) tooling to verify the control data and packet structure.
The protocol data is capture-derived from the original remote, so the payload fields and timings should be treated as empirically validated rather than a complete manufacturer specification.

## Hardware Setup

### Required Components
1. **IR LED**: High-power IR LED, typically 940 nm
2. **IR LED Driver**: Transistor or MOSFET for current amplification
3. **Current Limiting Resistor**: Sized for the selected LED and supply voltage

### GPIO
- IR TX GPIO: GPIO21
- DS18B20 local temperature GPIO: GPIO2 (separate from the IR transmitter)
- Manual 38 kHz carrier generation
- Carrier: 38 kHz
- Use a driver circuit instead of powering the IR LED directly from the ESP32-C6 pin.

## Protocol

The Ströme AC YPS-12C uses a Trotec 3550-compatible state-based protocol: every command transmits the complete AC state.

- Encoding: Pulse-distance
- Data length: 72 bits, 9 bytes
- Bit order: MSB first per byte in the validated ESP32-C6 transmitter
- Header mark: 12005 us
- Header space: 5130 us
- Bit mark: 545 us
- One space: 1950 us
- Zero space: 500 us
- Stop mark: 550 us
- Checksum: 8-bit sum of bytes 0 through 7 stored in byte 8

## Frame Fields

`main/trotec_3550_ir.cpp` builds frames from `ac_device_state_t`:

- Byte 0: Fixed intro `0x55`
- Byte 1: Swing, power, timer disabled, Celsius temperature offset
- Byte 2: Timer disabled
- Byte 3: Fahrenheit temperature offset
- Byte 6: Trotec native mode and fan speed
- Byte 7: Celsius unit flag
- Byte 8: Checksum

Known reserved bits are preserved from IRremoteESP8266's Trotec 3550 reset frame.

## Matter Mapping

- **OnOff cluster** sends power on/off while preserving the rest of the AC state.
- **Thermostat SystemMode** currently accepts Off and Cool only.
- **OccupiedCoolingSetpoint** accepts 16.00 C to 30.00 C.
- Fan speed, fan-only mode, dry mode, and vertical swing are present in the IR payload but not exposed over Matter in the current minimal Room Air Conditioner phase.

## Testing

1. Use an IR receiver or logic analyzer to capture GPIO21 output through the IR LED driver.
2. Confirm the captured frame is 72 bits, MSB first per byte, with the timings above.
3. Verify the checksum equals the 8-bit sum of the first 8 bytes.
4. Test with the Ströme AC YPS-12C for power, mode, fan, swing, and temperature changes.

The transmitter logs the generated frame:

```text
I (...) trotec_3550_ir: TX state: power=on mode=3 fan=2 swing=0 temp=24.00C frame=55 ...
```

## Troubleshooting

1. **No IR output**: Check GPIO21 wiring and the transistor/MOSFET driver.
2. **Weak range**: Increase LED current within safe limits or use multiple LEDs.
3. **Timing mismatch**: Capture with a logic analyzer and compare against the protocol timings.
4. **AC does not respond**: Verify the AC is a Ströme AC YPS-12C or Trotec 3550-compatible model and compare the emitted bytes with the original remote.
