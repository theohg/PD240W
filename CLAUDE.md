# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**PD240W** is an **adjustable power supply for motor drives** using USB-C Power Delivery negotiation, supporting up to **240W at 48V 5A**. The firmware runs on a Raspberry Pi Pico (RP2040) and provides:

- **User-adjustable voltage selection** from available USB-C PD contracts (up to 48V)
- **User-adjustable current limiting** (0-5A, monitored by INA228)
- **LCD menu interface** with 2 buttons + rotary encoder navigation
- **Real-time power monitoring** (voltage, current, power, temperature)
- **Safety features:** Overcurrent protection, optional overtemperature protection
- **Optional 17V buck converter** control (GPIO-enabled on PCB)

**Tech Stack:**
- Target: Raspberry Pi Pico (RP2040 microcontroller)
- Language: C++17
- SDK: Raspberry Pi Pico SDK v2.2.0
- Build: CMake + Ninja
- Communication: I2C (400kHz), SPI (10MHz), UART (115200 baud)

**Project Status:** Phase 1 & 2 complete. Ready for Phase 3 (Application Logic). See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for roadmap.

**Hardware Details:**
- **LCD:** 240x320 (2.4") ST7789, model HS20HS072RX
- **17V Buck:** Provides STO/SBC voltage for motor drive safety (200mA fused), enable only when VBUS > 18V
- **ADC Voltage:** Pre-switch VBUS measurement (150kΩ/10kΩ divider), redundant to INA228 post-switch
- **ADC Temp:** Onboard NTC thermistor (Beta=3950, 10kΩ @ 25°C, 4.7kΩ series resistor)
- **Current Resolution:** Target 1mA (0.001A) precision for user adjustment
- **Startup Behavior:** Output disabled by default, user must enable via button

## Build Commands

```bash
# Build
cd build && cmake .. && ninja

# Flash: Hold BOOTSEL while connecting USB, drag build/PD240W.uf2 to mounted drive

# Clean build
cd build && rm -rf * && cmake .. && ninja

# Serial debugging (UART on GP16/GP29 @ 115200)
screen /dev/tty.usbserial-* 115200

# EEPROM flashing (TPS26750 config update)
# 1. Set ENABLE_EEPROM_FLASHING to 1 in src/eeprom_loader.h
# 2. Rebuild and flash
# 3. Power cycle TPS26750 after successful flash
# 4. Set ENABLE_EEPROM_FLASHING back to 0 and rebuild
```

**Build Outputs:** `build/PD240W.elf`, `build/PD240W.uf2`, `build/compile_commands.json`

## Architecture

### Hardware Singleton Pattern
All hardware is accessed through a global singleton `hw` defined in `hardware.h`:

```cpp
extern Hardware hw;  // Global instance
```

The `Hardware` struct aggregates all drivers and is initialized once in `main()` via `hw.init()`.

### Initialization Sequence
1. `hw.init()` - Initializes all hardware (I2C, SPI, drivers)
2. `Interrupts::init()` - Setup all GPIO interrupts (encoder, overcurrent, USB-PD)
3. Main loop starts - Non-blocking event polling

### Main Event Loop Pattern
The main loop is **entirely non-blocking** using:
- Polling for button presses (`isPressed()`, `isClicked()`)
- Absolute time timers (`absolute_time_t`, `make_timeout_time_ms()`)
- State machines with internal timers (LED blinking, buzzer tones)
- **Must call `hw.update()`** each iteration for RGB LED and debug LED blinking to work

**Never use `sleep_ms()` or blocking delays in the main loop.**

### Interrupt Architecture
All GPIO interrupts are centralized in `interrupts.h/cpp`:

```cpp
namespace Interrupts {
    void init();                  // Setup all interrupts
    bool handleOvercurrent();     // Check/clear overcurrent flag
    bool handlePdInterrupt();     // Check/clear PD interrupt flag
}
```

**RP2040 Limitation:** Only ONE gpio callback for ALL pins. The interrupt module provides a single router that dispatches to individual handlers.

**ISR Safety Rules:**
- NEVER do I2C/SPI inside ISRs (use volatile flags, handle in main loop)
- Exception: Overcurrent protection cuts power immediately (safety-critical)

## Directory Structure

```
src/
├── main.cpp              # Entry point, event loop
├── hardware.h/cpp        # Global Hardware singleton
├── interrupts.h/cpp      # GPIO interrupt handling
├── board_config.h        # Pin definitions and constants (Board:: namespace)
├── eeprom_loader.h/cpp   # TPS26750 EEPROM flashing utility (I2C1)
├── drivers/
│   ├── gpio/             # SimpleIO wrapper (digital I/O with blink support)
│   ├── input/            # Button, RotaryEncoder, ADC
│   ├── buzzer/           # Buzzer (PWM-based)
│   ├── rgb_led/          # SK6812 RGB LED (PIO-based)
│   ├── display/          # ST7789 LCD (SPI)
│   └── power/
│       ├── ina228/       # Power monitor (I2C, 0x40)
│       └── tps26750/     # USB PD controller (I2C, 0x21)
├── utils/                # Utility functions (logging.h)
└── ui/                   # Display manager (Phase 4 in progress)
    └── display_manager.cpp
```

## Key Subsystems

### USB Power Delivery (TPS26750)
**Purpose:** Negotiates voltage contracts with USB-C chargers (5V-48V, including PPS and EPR AVS)

**Key API:**
```cpp
// Core
bool init();
bool getMode(char* modeStr);              // "APP ", "BOOT", "PTCH"

// Contract Discovery & Monitoring
uint8_t getSourceCapabilities(SourceCapability* caps, uint8_t max_caps);
bool getActiveContract(uint32_t& voltage_mv, uint32_t& current_ma);

// Voltage Negotiation
bool requestFixedProfile(uint32_t voltage_mv, uint32_t max_current_ma);
bool requestPPSProfile(uint32_t voltage_mv, uint32_t current_ma);
bool requestAVSProfile(uint32_t voltage_mv, uint32_t current_ma);

// Interrupts
bool readInterrupts(uint8_t* events);
bool clearInterrupts(const uint8_t* mask);
bool isInterruptSet(const uint8_t* buffer, uint8_t bitIndex);
```

**SourceCapability Struct:**
```cpp
struct SourceCapability {
    uint32_t voltage_mv;      // Fixed: Voltage. PPS/AVS: Max Voltage
    uint32_t max_current_ma;
    bool is_pps;              // Programmable Power Supply (SPR, 5-21V)
    bool is_avs;              // Adjustable Voltage Supply (EPR, 15-48V)
    uint32_t min_voltage_mv;  // Min voltage (PPS/AVS only)
};
```

**Negotiation Process:**
1. Read available contracts with `getSourceCapabilities()`
2. Call appropriate `request*Profile()` function
3. Monitor `INT_EVENT1` bit 12 (NEW_CONTRACT_AS_SINK) for completion
4. Verify with `getActiveContract()`

### TPS26750 EEPROM Flashing
**Purpose:** Program TPS26750 configuration patch to external EEPROM (CAT24C512) via I2C1

The TPS26750 loads its configuration from EEPROM at boot. The RP2040 can program this EEPROM directly:
- **I2C1:** GP14 (SDA), GP15 (SCL) at 400kHz
- **EEPROM:** CAT24C512 (64KB, 128-byte pages) at address 0x50
- **Binary:** `full_flash_c_26_11.c` contains the TPS26750 configuration array
- **Enable:** Set `ENABLE_EEPROM_FLASHING` to 1 in `eeprom_loader.h`

**Important:** Flashing is disabled by default. Only enable when updating TPS26750 config.

### Power Monitoring & Safety (INA228)
- Measures voltage, current, power, temperature
- Configured with 8mΩ shunt resistor, 5A max
- Overcurrent protection via latched ALERT interrupt
- On alert: Load switch disabled immediately in ISR
- Recovery: User clears latch via encoder button

**Key GPIOs:**
- `hw.loadSwitch` (GPIO 3) - Enables/disables output
- `hw.overcurrentAlert` (GPIO 12) - INA228 ALERT pin (active low, latched)
- `hw.EN_17V` (GPIO 20) - Optional 17V rail

### Input Handling
- **Button:** Hardware debouncing (50ms), `isPressed()` / `isClicked()`
- **RotaryEncoder:** ISR-based quadrature decoding, `getTicks()` / `reset()`
- **ADC:** Voltage (GP26) and temperature (GP27) with NTC conversion

### Display (ST7789)
- 240x320, SPI @ 10MHz, 180° rotation (MADCTL 0xC0)
- Functions: `fillScreen()`, `drawPixel()`, `drawLine()`, `drawRect()`, `fillRect()`
- Text: `drawChar()`, `drawString()`, `drawInt()`, `drawFloat()` with 5x7 font

### RGB LED (SK6812)
- PIO-based driver for precise 800kHz timing
- `setColor(r, g, b, brightness)`, `startBlink(interval_ms, duration_ms)`
- Must call `update()` in main loop

### Buzzer
- PWM-based, 100Hz-10kHz
- `playTone(freq, duration_ms)`, `playMelody(notes, length)`
- Non-blocking via timer callbacks

### SimpleIO (GPIO Wrapper)
- `on()`, `off()`, `toggle()`, `read()`
- `startBlink(interval_ms, duration_ms)`, `stopBlink()`, `update()`

## Configuration

All constants are in the `Board::` namespace in `board_config.h`:

**Pin Definitions:**
```cpp
Board::PIN_BTN_1, PIN_BTN_2, PIN_ENC_BTN
Board::PIN_I2C_SDA, PIN_I2C_SCL     // I2C0
Board::PIN_LCD_CS, PIN_LCD_DC, etc. // SPI display
Board::PIN_RGB_LED                  // PIO
Board::I2C_ADDR_INA228              // 0x40
Board::I2C_ADDR_TPS26750            // 0x21
```

**Hardware Constants:**
```cpp
Board::ADC_REF_VOLTAGE           // 3.3V
Board::NTC_BETA                  // 3950
Board::VOLTAGE_DIVIDER_TOP/BOT   // 150kΩ/10kΩ
Board::INA228_SHUNT_RESISTOR     // 8mΩ
Board::INA228_MAX_CURRENT        // 5A
```

**UART:** TX=GP16, RX=GP29 (configured in CMakeLists.txt)

## Communication Buses

| Bus | Pins | Frequency | Devices |
|-----|------|-----------|---------|
| I2C0 | GP4, GP5 | 400 kHz | INA228, TPS26750 |
| I2C1 | GP14, GP15 | 400 kHz | CAT24C512 EEPROM (TPS26750 config) |
| SPI0 | GP18, GP19 | 10 MHz | ST7789 Display |
| UART0 | GP16, GP29 | 115200 | Debug console |
| PIO | GP28 | 800 kHz | SK6812 RGB LED |
| ADC | GP26, GP27 | On-demand | Voltage/temperature |
| PWM | GP6 | Variable | Buzzer |

## Adding New Components

### New Input Device
1. Create driver in `src/drivers/input/`
2. Add member to `Hardware` struct
3. Initialize in `Hardware::init()`
4. Poll in main loop or add ISR handler in `interrupts.cpp`

### New I2C Peripheral
1. Create driver with `i2c_inst_t *_i2c` member
2. Use `i2c_read_blocking()` / `i2c_write_blocking()`
3. **Never do I2C inside ISRs** - set flags, handle in main loop

### New PIO-Based Driver
1. Write PIO assembly in `.pio` file
2. Add to CMakeLists.txt: `pico_generate_pio_header()`
3. Load program and claim state machine in driver

## Important Patterns

### Non-Blocking Timing
```cpp
static absolute_time_t next_event = make_timeout_time_ms(1000);
if (absolute_time_diff_us(get_absolute_time(), next_event) < 0) {
    // Execute
    next_event = make_timeout_time_ms(1000);
}
```

### Static Wrappers for C Callbacks
```cpp
class Buzzer {
    static void stopToneCallback(void *param) {
        static_cast<Buzzer*>(param)->stopTone();
    }
};
```

## Best Practices

### Mandatory Rules
1. **No Global Variables** - Use `hw` singleton only
2. **No Blocking Delays** in main loop
3. **Keep Drivers Hardware-Focused** - No business logic
4. **YAGNI** - Implement only what's needed now

### Error Handling
- Driver functions should return `bool` status
- Use `LOG_HW_INIT()` macro for init functions
- Use logging macros: `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_DEBUG`

### Naming Conventions
- Classes: `PascalCase`
- Functions: `camelCase`
- Variables: `snake_case`
- Constants: `UPPER_SNAKE_CASE`
- Private members: `_leading_underscore`

### Driver Independence
- Testable in isolation
- No dependencies on other drivers
- No project-specific includes (like logging) in drivers
- Handle own error conditions

## Debugging

### Common Issues
1. **I2C not responding**: Check pull-ups (4.7kΩ to 3.3V)
2. **Display not updating**: Verify SPI pins and CS/DC/RST signals
3. **Encoder not counting**: Check ISR routing in `interrupts.cpp`
4. **LED wrong colors**: Verify GRB order
5. **Overcurrent false triggers**: Adjust threshold or check shunt value

### VSCode Environment
Pico SDK extension sets: `PICO_SDK_PATH`, `PICO_TOOLCHAIN_PATH`, CMake, Ninja, Picotool

## Development Roadmap

| Phase | Status | Description |
|-------|--------|-------------|
| 1. Hardware Foundation | ✅ Complete | All drivers working |
| 2. TPS26750 USB PD | ✅ Complete | Full PD negotiation |
| 3. Application Logic | Pending | State machine, settings, safety |
| 4. User Interface | Pending | LCD menu, input handling |
| 5. Integration & Testing | Pending | End-to-end testing |

## Key Files

| File | Purpose |
|------|---------|
| main.cpp | Entry point, event loop |
| hardware.h/cpp | Global singleton, component instances |
| interrupts.h/cpp | GPIO interrupt handling |
| board_config.h | Pin definitions and constants |
| eeprom_loader.h/cpp | TPS26750 EEPROM flashing (enable via `ENABLE_EEPROM_FLASHING`) |

## Safety-Critical Code

**Overcurrent Protection** (in `interrupts.cpp`):
- Triggered by INA228 ALERT pin (active-low, latched)
- Immediately disables load switch (no delays)
- Sets red LED for visual indication
- Must clear latch via `getDiagnoseAlert()` before re-enable

**Do not modify** without understanding electrical safety implications.

## Historical Bug Fixes

**Button Debouncing:** Static variable was shared across instances, causing multi-triggers. Fixed with per-instance `was_pressed_for_click` member.

**LCD Display:** Minimal init sequence missing MADCTL, gamma, power control. Fixed with complete ST7789 initialization.

**Lesson:** Never use static variables for instance state. Always use complete init sequences from datasheets.
