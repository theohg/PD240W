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
- Communication: I2C (400kHz), SPI (24MHz), UART (115200 baud)

**Project Status:** Phase 1 (Hardware Foundation) in progress. See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for detailed roadmap.

**Hardware Details:**
- **LCD:** 240x320 (2.4") ST7789, model HS20HS072RX
- **17V Buck:** Provides STO/SBC voltage for motor drive safety (200mA fused), enable only when VBUS > 18V
- **ADC Voltage:** Pre-switch VBUS measurement (150kΩ/10kΩ divider), redundant to INA228 post-switch
- **ADC Temp:** Onboard NTC thermistor (Beta=3950, 10kΩ @ 25°C, 4.7kΩ series resistor)
- **Current Resolution:** Target 1mA (0.001A) precision for user adjustment
- **Startup Behavior:** Output disabled by default, user must enable via button

## Build Commands

### Initial Setup
```bash
# The Pico SDK and toolchain are managed via VSCode extension
# Environment variables are set automatically in the terminal
# Verify environment:
echo $PICO_SDK_PATH  # Should show ~/.pico-sdk/sdk/2.2.0
```

### Building
```bash
cd build
cmake ..
make -j4
# Or using Ninja (default in VSCode):
ninja
```

### Build Outputs
- `build/PD240W.elf` - ELF executable
- `build/PD240W.uf2` - Flashable UF2 file (drag to BOOTSEL mount)
- `build/PD240W.bin` - Raw binary
- `build/compile_commands.json` - For IDE integration

### Flashing
1. Hold BOOTSEL button on Pico while connecting USB
2. Drag `build/PD240W.uf2` to the mounted drive
3. Pico will reboot automatically

### Clean Build
```bash
cd build
rm -rf *
cmake ..
make -j4
```

### Serial Debugging
```bash
# UART is on custom pins (TX=GP16, RX=GP29) at 115200 baud
# Use any serial terminal:
screen /dev/tty.usbserial-* 115200
# or
minicom -D /dev/tty.usbserial-* -b 115200
```

## Architecture

### Hardware Singleton Pattern
All hardware is accessed through a global singleton `hw` defined in [hardware.h](src/hardware.h):

```cpp
extern Hardware hw;  // Global instance
```

The `Hardware` struct aggregates all drivers and is initialized once in `main()` via `hw.init()`. All components (buttons, LEDs, sensors, displays) are members of this struct.

### Initialization Sequence
From [main.cpp](src/main.cpp):

1. `hw.init()` - Initializes all hardware (I2C, SPI, drivers)
2. `gpio_set_irq_enabled_with_callback()` - Setup encoder interrupt with router callback
3. `gpio_set_irq_enabled()` - Enable second encoder pin and overcurrent interrupt
4. Main loop starts - Non-blocking event polling

### Main Event Loop Pattern
The main loop is **entirely non-blocking** using:
- Polling for button presses (`isPressed()`, `isClicked()`)
- Absolute time timers for periodic tasks (`absolute_time_t`, `make_timeout_time_ms()`)
- State machines with internal timers (LED blinking, buzzer tones)

**Never use `sleep_ms()` or blocking delays in the main loop.**

### GPIO Interrupt Routing
From [main.cpp:26-36](src/main.cpp#L26-L36):

```cpp
void gpio_callback(uint gpio, uint32_t events) {
    // Route encoder pins
    if (hw.encoder.isMyPin(gpio)) {
        hw.encoder.handleISR(gpio, events);
    }

    // Route overcurrent alert
    if (gpio == Board::PIN_SWITCH_EN_READ) {
        over_current(gpio, events);
    }
}
```

All GPIO interrupts go through this router. To add a new interrupt-driven input:
1. Add the handler logic to `gpio_callback()`
2. Enable the interrupt with `gpio_set_irq_enabled()` in `main()`

## Directory Structure

```
src/
├── main.cpp              # Entry point, event loop, ISR router
├── hardware.h/cpp        # Global Hardware singleton
├── config.h              # Constants (ADC, NTC, voltage divider)
│
├── bsp/                  # Board Support Package
│   ├── pin_map.h         # GPIO pin definitions (Board namespace)
│   └── board_init.cpp    # Early hardware init (I2C, SPI, UART)
│
├── drivers/              # Hardware drivers by category
│   ├── gpio/             # SimpleIO wrapper (digital I/O)
│   ├── input/            # Button, RotaryEncoder, ADC
│   ├── output/           # Buzzer (PWM-based)
│   ├── rgb_led/          # SK6812 RGB LED (PIO-based)
│   ├── display/          # ST7789 LCD (SPI)
│   └── power/
│       ├── ina228/       # Power monitor (I2C, 0x40)
│       └── tps26750/     # USB PD controller (I2C, 0x20) [core complete]
│
├── hal/                  # Hardware abstraction (I2C bus wrapper)
└── ui/                   # Display manager (minimal/reserved)
```

## Key Subsystems

### USB Power Delivery (TPS26750)
**TPS26750 USB PD Controller** ([drivers/power/tps26750/](src/drivers/power/tps26750/)):
- **Status:** ✅ Core implementation complete - Phase 2 in progress
- **Purpose:** Negotiates voltage contracts with USB-C chargers (5V-48V, including PPS)
- **I2C Address:** 0x20 (configurable via ADCINx pins)
- **Protocol:** Custom "Unique Address Interface" with byte-count prefix (implemented)

**Implemented Features:**
- ✅ Full I2C register protocol (read/write with byte-count handling)
- ✅ Complete register map (29 registers: MODE, STATUS, PDO, RDO, interrupts, etc.)
- ✅ 4CC command support (Gaid, SWSk, GSrC, GSkC for warm/cold reset, role swap, cap discovery)
- ✅ Interrupt handling (read/clear/check 11-byte INT_EVENT1 register)
- ✅ Active contract reading (`getActiveContract()` - supports Fixed and PPS PDOs)
- ✅ Source capability discovery (`getSourceCapabilities()` - parses charger's offered contracts)
- ✅ PDO parsing for Fixed Supply (voltage in 50mV units, current in 10mA units)
- ✅ PDO parsing for PPS/Augmented (voltage in 20mV units, current in 50mA units)
- ✅ Device mode detection (APP/BOOT/PTCH)

**Remaining Work:**
- Request specific voltage (send RDO to negotiate contract)
- Monitor connection status and handle disconnect/reconnect
- Full interrupt-driven event handling
- Current limit advertisement configuration

**Key API Methods:**
```cpp
bool init()                                      // Verify device presence
bool getMode(char* modeStr)                      // Read MODE register ("APP ", "BOOT")
bool sendCommand(const char* cmd)                // Send 4CC command
bool getActiveContract(uint32_t& voltage_mv, uint32_t& current_ma)
uint8_t getSourceCapabilities(SourceCapability* caps, uint8_t max_caps)
bool readInterrupts(uint8_t* events)             // Read 11-byte interrupt buffer
bool clearInterrupts(const uint8_t* mask)        // Clear specific interrupts
```

**SourceCapability Struct:**
```cpp
struct SourceCapability {
    uint32_t voltage_mv;      // Voltage in millivolts
    uint32_t max_current_ma;  // Max current in milliamps
    bool is_pps;              // True if Programmable Power Supply (variable voltage)
    uint32_t min_voltage_mv;  // Min voltage (PPS only)
};
```

### Power Monitoring & Safety
**INA228 Power Monitor** ([drivers/power/ina228/](src/drivers/power/ina228/)):
- Measures voltage, current, power, temperature
- Configured with 8mΩ shunt resistor, 5A max
- **Overcurrent threshold: Configurable** (currently 40mA for testing) with latched ALERT interrupt
- On alert: `hw.loadSwitch.off()` called immediately in ISR
- Recovery: User presses encoder button → clears latch via I2C → re-enables load
- **Production use:** Threshold will be set to user-selected current limit

**Critical:** The overcurrent handler in [main.cpp:12-23](src/main.cpp#L12-L23) is **safety-critical**. It immediately cuts power before any other action.

**Load Switch Control:**
- `hw.loadSwitch` (GPIO 3) - Enables/disables output to load
- Status readable via `PIN_SWITCH_EN_READ` (GPIO 12)
- Controlled by safety logic and user commands

**17V Buck Converter:**
- `hw.EN_17V` (GPIO 20) - Enables optional 17V rail on PCB
- Purpose: Powers internal circuitry or provides auxiliary output
- Can be controlled through user interface or automatically

### Input Handling
**Button** ([drivers/input/button.h](src/drivers/input/button.h)):
- Hardware debouncing (50ms default)
- Two modes: `isPressed()` (hold state), `isClicked()` (edge detection)
- Supports active-high/low and pull-up/down

**RotaryEncoder** ([drivers/input/rotary_enc.h](src/drivers/input/rotary_enc.h)):
- ISR-based quadrature decoding with built-in debouncing
- Call `handleISR()` from global `gpio_callback()`
- Read `getTicks()` for cumulative rotation
- Call `reset()` to zero the count
- Status: Fully implemented with debouncing

**ADC Inputs** ([drivers/input/adc_inputs.h](src/drivers/input/adc_inputs.h)):
- GP26 (ADC0) - VBUS voltage measurement via 150kΩ/10kΩ divider (pre-switch)
- GP27 (ADC1) - Temperature via NTC thermistor (10kΩ @ 25°C, Beta=3950, 4.7kΩ series)
- Provides redundant voltage measurement (cross-check with INA228) and thermal monitoring
- Status: Fully implemented with Steinhart-Hart NTC conversion

### Display System & User Interface
**ST7789 LCD** ([drivers/display/st7789.h](src/drivers/display/st7789.h)):
- SPI-based communication (24MHz)
- Control pins: CS, DC (data/command), RST, BL (backlight)
- Resolution: 240x320 (2.4" display, model HS20HS072RX)
- Display orientation: 180° rotation configured (MADCTL = 0xC0)
- Full initialization with gamma correction, power control, and display inversion
- Implemented functions: `init()`, `fillScreen()`, `drawPixel()`, `drawLine()`, `drawRect()`, `fillRect()`
- Text rendering: `drawChar()`, `drawString()`, `drawInt()`, `drawFloat()` with 5x7 font
- Status: Fully functional with complete ST7789 initialization sequence

**User Interface Concept:**
- **2 Buttons:** Menu navigation (BTN1=enter/back, BTN2=select/enable output)
- **Rotary Encoder:** Value adjustment and menu scrolling
- **Encoder Button:** Confirm selections, clear faults
- **RGB LED:** Status indication (green=OK, yellow=warning, red=fault)
- **Buzzer:** Button feedback and startup melody

**Note:** [display_manager.cpp](src/ui/display_manager.cpp) and [src/ui/screens/](src/ui/screens/) are reserved for Phase 4 development. See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for UI implementation strategy.

### RGB LED (SK6812)
**PIO-Based Driver** ([drivers/rgb_led/sk6812.h](src/drivers/rgb_led/sk6812.h)):
- Uses PIO (Programmable I/O) for precise 800kHz timing
- PIO program defined in [sk6812.pio](src/drivers/rgb_led/sk6812.pio)
- Non-blocking blink with duration: `startBlink(interval_ms, duration_ms)`
- Must call `update()` in main loop for timing
- Color format: GRB order (green, red, blue)

### Buzzer
**PWM-Based Driver** ([drivers/buzzer/buzzer.h](src/drivers/buzzer/buzzer.h)):
- Frequency range: 100Hz - 10kHz
- Non-blocking tone playback using timer callbacks
- Methods: `setFrequency()`, `playTone(freq, duration_ms)`, `playMelody(notes, length)`
- Melody playback implemented (Mario power-up sound plays on startup)
- Status: Fully implemented

### SimpleIO (GPIO Wrapper)
**SimpleIO Class** ([drivers/gpio/gpio.h](src/drivers/gpio/gpio.h)):
- Basic digital I/O abstraction
- Methods: `on()`, `off()`, `toggle()`, `read()`
- Used for: debug LED, load switch, 17V buck enable
- Status: Fully implemented

## Configuration

### Pin Definitions
Edit [src/bsp/pin_map.h](src/bsp/pin_map.h) to change GPIO assignments. All pins are in the `Board::` namespace:

```cpp
Board::PIN_BTN_1       // GPIO 0
Board::PIN_I2C_SDA     // GPIO 4 (I2C0)
Board::PIN_BUZZER      // GPIO 6
Board::PIN_RGB_LED     // GPIO 28 (PIO)
Board::PIN_LCD_CS      // GPIO 17 (SPI1)
```

**UART is on custom pins:** TX=GP16, RX=GP29 (configured in [CMakeLists.txt:61-66](CMakeLists.txt#L61-L66))

### Constants
Edit [src/config.h](src/config.h) for calibration values:

```cpp
Config::ADC_REF_VOLTAGE          // 3.3V
Config::NTC_BETA                 // 3950 (thermistor)
Config::VOLTAGE_DIVIDER_TOP      // 150kΩ
Config::LED_FREQ                 // 800kHz (WS2812)
```

### INA228 Overcurrent Threshold
Modify in [hardware.cpp](src/hardware.cpp) `Hardware::init()`:

```cpp
powerMonitor.setAlertConfig(
    INA228_ALERT_CONV_READY,    // Alert on conversion ready
    false,                       // Not latched by default
    true,                        // Active low (for interrupt)
    INA228_ALERT_THRESHOLD_OVER_CURRENT,
    40.0f                        // <-- Threshold in mA
);
```

## Communication Buses

| Bus | Pins | Frequency | Devices |
|-----|------|-----------|---------|
| **I2C0** | GP4 (SDA), GP5 (SCL) | 400 kHz | INA228 (0x40), TPS26750 (0x20) |
| **SPI1** | GP18 (SCK), GP19 (MOSI) | 24 MHz | ST7789 Display |
| **UART0** | GP16 (TX), GP29 (RX) | 115200 | Debug console |
| **PIO** | GP28 | 800 kHz | SK6812 RGB LED |
| **ADC** | GP26, GP27 | On-demand | Voltage/temperature |
| **PWM** | GP6 | Variable | Buzzer |

## Adding New Components

### New Input Device
1. Create driver in `src/drivers/input/yourdevice.h/cpp`
2. Add member to `Hardware` struct in [hardware.h](src/hardware.h)
3. Initialize in `Hardware::init()` in [hardware.cpp](src/hardware.cpp)
4. Poll in main loop or setup ISR in [main.cpp](src/main.cpp)

### New I2C Peripheral
1. Create driver with `i2c_inst_t *_i2c` member
2. Pass `i2c0` from `Hardware::init()`
3. Use `i2c_read_blocking()` / `i2c_write_blocking()`
4. **Never do I2C inside ISRs** - set flags and handle in main loop

### New PIO-Based Driver
1. Write PIO assembly in `.pio` file
2. Add to `CMakeLists.txt`: `pico_generate_pio_header(PD240W ${CMAKE_CURRENT_LIST_DIR}/src/your.pio)`
3. Load program in driver: `pio_add_program(pio, &program)`
4. Claim state machine: `pio_claim_unused_sm(pio, true)`

## Important Patterns

### Non-Blocking Timing
Always use absolute time for periodic tasks:

```cpp
static absolute_time_t next_event = make_timeout_time_ms(1000);
if (absolute_time_diff_us(get_absolute_time(), next_event) < 0) {
    // Time to execute
    next_event = make_timeout_time_ms(1000);  // Schedule next
}
```

### Static Wrappers for C Callbacks
When using C++ methods with C-style callbacks (timers, repeating timers):

```cpp
class Buzzer {
    static void stopToneCallback(void *param) {
        Buzzer *self = static_cast<Buzzer*>(param);
        self->stopTone();
    }
};
```

### Active-Low Logic
Many peripherals use active-low signals (buttons, interrupts). The Button driver handles this with `_active_high` flag. Check polarity when debugging unexpected behavior.

## Best Practices & Code Principles

### Mandatory Practices ⚠️
These rules MUST be followed in all code:

1. **No Global Variables** - All hardware access through `hw` singleton only
   - ❌ `int global_counter;` outside functions
   - ✅ Add members to `Hardware` struct if needed

2. **No Blocking Delays** - Main loop must remain non-blocking
   - ❌ `sleep_ms(1000);` in main loop
   - ✅ Use `absolute_time_t` and `make_timeout_time_ms()`

3. **Keep Drivers Hardware-Focused** - No business logic in drivers
   - ❌ Button driver checking "if voltage > 20V then..."
   - ✅ Button driver only handles debouncing and state

4. **Don't Over-Engineer** - YAGNI (You Aren't Gonna Need It)
   - ❌ Adding configuration for every possible future use case
   - ✅ Implement exactly what's needed now, refactor later if needed

### Error Handling Standards

**All driver functions should return status:**
```cpp
// Good - Returns success/failure
bool init() {
    if (hardware_init_failed()) return false;
    return true;
}

// Acceptable for simple setters
void setColor(uint8_t r, uint8_t g, uint8_t b) {
    // No failure modes
}

// Bad - Silent failure
void init() {
    if (hardware_init_failed()) {
        // Fails silently, caller doesn't know
    }
}
```

**Use logging for debugging:**
```cpp
if (!sensor.init()) {
    LOG_ERROR("INA228 initialization failed");
    return false;
}
```

### Logging System

Use the logging macros defined in `src/utils/logging.h`:

```cpp
LOG_INFO("System started");              // Always shown
LOG_WARN("Temperature high: %.1f°C", t); // Warnings
LOG_ERROR("I2C timeout on address 0x%02X", addr); // Errors
LOG_DEBUG("Raw ADC: %d", adc_val);       // Debug builds only
```

**Guidelines:**
- Use `LOG_INFO` for significant events (startup, state changes)
- Use `LOG_WARN` for concerning but non-fatal issues
- Use `LOG_ERROR` for failures that affect functionality
- Use `LOG_DEBUG` for verbose debugging (disabled in production)
- Include relevant values in messages (voltages, addresses, counts)

### Configuration Validation

**Use static_assert for compile-time checks:**
```cpp
static_assert(Config::NTC_BETA > 0, "NTC Beta must be positive");
static_assert(Config::VOLTAGE_DIVIDER_TOP > Config::VOLTAGE_DIVIDER_BOT,
              "Voltage divider ratios incorrect");
```

**Use runtime checks in init():**
```cpp
bool Hardware::init() {
    if (Config::ADC_REF_VOLTAGE <= 0) {
        LOG_ERROR("Invalid ADC reference voltage");
        return false;
    }
    // ... rest of init
}
```

### Code Style Guidelines

**Naming Conventions:**
- Classes/Structs: `PascalCase` (e.g., `INA228`, `Hardware`)
- Functions/Methods: `camelCase` (e.g., `getBusVoltage()`, `init()`)
- Variables: `snake_case` (e.g., `last_tick`, `adc_value`)
- Constants: `UPPER_SNAKE_CASE` (e.g., `PIN_BTN_1`, `MAX_VOLTAGE`)
- Private members: `_leading_underscore` (e.g., `_i2c`, `_pin`)

**Comments:**
- Comment WHY, not WHAT (code should be self-documenting)
- Complex algorithms need explanation (e.g., NTC temperature conversion)
- Safety-critical code needs warnings
- TODOs should include reason: `// TODO: Add timeout handling - current impl blocks`

**Example - Good vs Bad:**
```cpp
// ❌ Bad - Comments obvious code
// Set the pin to high
gpio_put(pin, 1);

// ✅ Good - Explains WHY
// Enable load switch after fault is cleared
hw.loadSwitch.on();

// ✅ Good - Complex algorithm explained
// Steinhart-Hart equation for NTC thermistor
// T = 1 / (A + B*ln(R) + C*ln(R)^3)
// Simplified Beta parameter equation used here
float temp = 1.0 / (1.0/T0 + (1.0/Beta) * log(R_ntc / R0)) - 273.15;
```

### Driver Independence

Each driver should:
- ✅ Be testable in isolation
- ✅ Have no dependencies on other drivers (except HAL)
- ✅ Not access global state (except `hw` singleton)
- ✅ Handle its own error conditions
- ✅ Provide clear API with minimal assumptions

### Safety-Critical Code Markers

Mark safety-critical sections clearly:
```cpp
// SAFETY-CRITICAL: Overcurrent protection - must execute immediately
void over_current(uint gpio, uint32_t events) {
    hw.loadSwitch.off();  // Cut power FIRST
    hw.rgbLed.setColor(255, 0, 0, 255);
    LOG_ERROR("OVERCURRENT DETECTED - LOAD DISABLED");
}
```

### Testing Approach

During Phase 1, test each component independently:
1. Write test code in `main()` - clearly commented
2. Test one feature at a time
3. Verify with serial output (logging)
4. Keep test code organized and easy to remove later

Example organization in main():
```cpp
// ========== HARDWARE TESTS (Phase 1) ==========
// Remove this section when moving to Phase 3

void test_gpio() {
    // GPIO input test code
}

void test_adc() {
    // ADC test code
}

// Call in main loop:
if (PHASE1_TESTING) {
    test_gpio();
    test_adc();
}
```

## Critical Bugs Fixed in Phase 1

### Button Debouncing Bug (CRITICAL)
**Problem:** All three buttons shared a single `static bool was_pressed` variable in `Button::isClicked()`, causing:
- Button presses to trigger 5-15 times per click
- Cross-talk between different button instances
- Unreliable input detection

**Root Cause:** Static variable in [button.cpp:58](src/drivers/input/button.cpp#L58) was shared across all Button instances.

**Fix:** Added per-instance state tracking:
- Added `bool was_pressed_for_click` member variable to Button class
- Each button now maintains its own edge detection state
- Debouncing now works correctly for all buttons independently

**Lesson:** Never use static variables for instance-specific state in C++ classes.

### LCD Display Not Working (FIXED)
**Problem:** Display backlight turned on but no content was visible (blank white/black screen).

**Root Cause:** Minimal ST7789 initialization sequence was missing critical configuration:
- No memory access control (MADCTL) setup
- Missing gamma correction tables
- No power control configuration
- Missing display inversion command

**Fix:** Implemented complete ST7789 initialization sequence in [st7789.cpp:11-127](src/drivers/display/st7789.cpp#L11-L127):
- Added software reset (SWRESET)
- Configured MADCTL for proper orientation (0xC0 for 180° rotation)
- Added porch, gate, and VCOM settings
- Programmed positive/negative gamma curves
- Enabled display inversion (INVON)
- Added proper timing delays between commands

**Lesson:** Always use complete initialization sequences from datasheets for complex peripherals like display controllers.

**Status:** ✅ FULLY WORKING - Display shows graphics and text correctly with proper orientation

### Output Enable Control
**Enhancement:** Encoder button now controls the load switch output:
- Press encoder button → toggles output on/off
- Automatically clears INA228 latched fault when pressed
- RGB LED changes color to indicate state (green=on, yellow=off, red=fault)
- Enables testing of the power path and overcurrent protection

## Debugging

### Serial Output
- `printf()` goes to UART0 (GP16/GP29 @ 115200 baud)
- Main loop prints power stats every 500ms
- Overcurrent events log immediately

### Common Issues
1. **I2C not responding**: Check pull-ups (usually 4.7kΩ to 3.3V)
2. **Display not updating**: Verify SPI pins and CS/DC/RST signals
3. **Encoder not counting**: Check interrupt routing in `gpio_callback()`
4. **LED wrong colors**: Verify GRB vs RGB order in driver
5. **Overcurrent false triggers**: Adjust threshold or check shunt resistor value

### VSCode Environment
The Pico SDK extension automatically sets:
- `PICO_SDK_PATH` → `~/.pico-sdk/sdk/2.2.0`
- `PICO_TOOLCHAIN_PATH` → `~/.pico-sdk/toolchain/14_2_Rel1`
- CMake, Ninja, Picotool in PATH

If builds fail, verify these environment variables are set in the integrated terminal.

## Development Roadmap

**Current Status:** Phase 1 Complete, Phase 2 In Progress

**See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for detailed implementation plan.**

### Phase 1: Hardware Foundation (✅ COMPLETE)
- ✅ GPIO input reading (SimpleIO with `read()` method)
- ✅ ADC reading with voltage divider and NTC conversion
- ✅ Encoder with debouncing implemented
- ✅ LCD graphics (text, shapes, numbers, float rendering)
- ✅ Buzzer melody playback (Mario power-up sound)
- ✅ Button debouncing fixed (per-instance state tracking)
- ✅ Encoder button controls output enable/disable and clears INA228 fault latch

### Phase 2: TPS26750 USB PD Integration (🔄 IN PROGRESS)
- ✅ Research TPS26750 register map and protocol
- ✅ Implement I2C communication with unique byte-count protocol
- ✅ PD contract discovery (parse Source Capabilities from charger)
- ✅ Active contract monitoring (read current voltage/current)
- ✅ Interrupt handling framework (read/clear/check)
- ✅ 4CC command support (device control)
- ⏳ Voltage request negotiation (send RDO to request specific voltage)
- ⏳ Connection status monitoring
- ⏳ Current limit configuration

### Phase 3: Application Logic (Pending)
- State machine implementation (BOOT, IDLE, RUNNING, MENU, FAULT, SETTING_*)
- Settings manager (voltage, current limit, 17V buck control)
- Safety logic (overtemp, overvoltage, current limit enforcement)
- Power statistics tracking

### Phase 4: User Interface (Pending)
- Screen layout design
- Display manager implementation
- Individual screen classes (main, menu, voltage select, current set, fault)
- Input handling (button/encoder to UI)
- Visual feedback (RGB LED states, buzzer feedback)

### Phase 5: Integration & Testing (Pending)
- End-to-end testing
- Edge case handling
- Performance optimization
- Code cleanup and documentation

**Phase 1 Hardware Testing Results:**
- ✅ All buttons working with proper debouncing
- ✅ Encoder tracking rotation with debouncing
- ✅ Encoder button now enables/disables output switch and clears INA228 fault
- ✅ ADC reading voltage and temperature correctly
- ✅ INA228 power monitoring operational
- ✅ RGB LED status indication working
- ✅ Buzzer plays Mario power-up melody on startup
- ✅ LCD display initialized with full ST7789 command sequence (gamma, power control, etc.)
- ✅ Text and graphics rendering functional

**Remaining Work for Future Phases:**
- TPS26750 voltage negotiation (Phase 2 - in progress)
- Application state machine (Phase 3)
- User interface screens and menu system (Phase 4)
- Integration testing (Phase 5)

## Key Files Reference

| File | Purpose |
|------|---------|
| [main.cpp](src/main.cpp) | Entry point, event loop, ISR router |
| [hardware.h](src/hardware.h) / [.cpp](src/hardware.cpp) | Global singleton, all component instances |
| [config.h](src/config.h) | Calibration constants (ADC, NTC, dividers) |
| [pin_map.h](src/bsp/pin_map.h) | All GPIO pin assignments |
| [CMakeLists.txt](CMakeLists.txt) | Build config, linked libraries, UART pins |

## Safety-Critical Code

**Overcurrent Protection** ([main.cpp:12-23](src/main.cpp#L12-L23)):
- Triggered by INA228 ALERT pin (active-low, latched)
- Immediately disables load switch (no delays)
- Sets red LED for visual indication
- Must clear latch via `getDiagnoseAlert()` before re-enable

**Do not modify** this handler without understanding the electrical safety implications. The load switch must be disabled as quickly as possible to prevent component damage.
