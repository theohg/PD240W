# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**PD240W** is an adjustable power supply for motor drives using USB-C Power Delivery negotiation, supporting up to 240W at 48V 5A. Firmware runs on a Raspberry Pi Pico (RP2040).

- **Target:** RP2040 (Raspberry Pi Pico), C++17, Pico SDK v2.2.0
- **Build:** CMake + Ninja
- **Status:** Phase 4 (UI refinement) in progress. See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for roadmap.
- **Key features:** Voltage selection from PD contracts (up to 48V), adjustable current limiting (0-5A via INA228), LCD menu with Prusa-style encoder navigation, overcurrent/overtemperature protection, optional 17V buck converter

## Build Commands

```bash
# Build (use -G Ninja explicitly)
cd build && cmake -G Ninja .. && ninja

# Clean build
cd build && rm -rf * && cmake -G Ninja .. && ninja

# Flash: Hold BOOTSEL while connecting USB, drag build/PD240W.uf2 to mounted drive

# Serial debugging (UART on GP16/GP29 @ 115200)
screen /dev/tty.usbserial-* 115200
```

**Build outputs:** `build/PD240W.uf2` (flash image), `build/PD240W.elf`, `build/compile_commands.json` (for IDE support)

**Build environment:** VSCode Pico SDK extension sets `PICO_SDK_PATH`, `PICO_TOOLCHAIN_PATH`, CMake, Ninja, Picotool. No test infrastructure exists.

## Architecture

### Global Singletons

Six global instances coordinate the application. All are defined as `extern` in their respective headers:

```cpp
extern Hardware hw;                // hardware.h - All hardware drivers
extern StateMachine stateMachine;  // logic/state_machine.h - Application state
extern Safety safety;              // logic/safety.h - Safety monitoring
extern PdManager pdManager;        // logic/pd_manager.h - PD negotiation
extern DisplayManager displayManager; // ui/display_manager.h - Screen rendering
extern Settings settings;          // logic/settings.h - User settings
```

### Initialization Sequence (in main.cpp)

1. `hw.init()` - I2C, SPI, all drivers (includes optional EEPROM flash)
2. `Interrupts::init()` - GPIO interrupts (encoder, overcurrent, USB-PD)
3. `settings.init()` / `safety.init()` / `pdManager.init()` / `displayManager.init()` / `stateMachine.init()`

### Main Event Loop (non-blocking, 5 steps)

```cpp
while (true) {
    bool needs_display_update = stateMachine.update();  // 1. Encoder, buttons, state transitions
    SafetyStatus status = safety.update();               // 2. Temp, voltage, current monitoring
    pdManager.update();                                  // 3. USB-PD interrupt processing
    if (needs_display_update || timer_elapsed)
        displayManager.render();                         // 4. Screen rendering (100ms or on change)
    hw.update();                                         // 5. RGB LED blink timing
}
```

**Never use `sleep_ms()` or blocking delays in the main loop.**

### Interrupt Architecture (interrupts.h/cpp)

All GPIO interrupts are centralized in a single router (RP2040 limitation: one gpio callback for ALL pins).

```cpp
namespace Interrupts {
    void init();
    bool handleOvercurrent();   // Check/clear volatile flag
    bool handlePdInterrupt();   // Check/clear volatile flag
}
```

**ISR safety rules:**
- NEVER do I2C/SPI inside ISRs - set volatile flags, handle in main loop
- Exception: Overcurrent protection cuts power immediately (safety-critical)

### State Machine

```
BOOT ──(3s timeout)──> MAIN
MAIN <──(long press)──> MENU     MAIN <──(fault)──> FAULT
MENU ──(select)──> ADJUST ──(confirm/back)──> MENU
FAULT ──(click acknowledge)──> MAIN
```

States: `BOOT`, `MAIN`, `MENU`, `ADJUST`, `FAULT` (see `AppState` enum in `state_machine.h`)

### Input Mapping (Prusa-Style)

| Control | Action |
|---------|--------|
| Encoder Rotate | Navigate menu / Adjust values |
| Encoder Click | Confirm / Select |
| Encoder Long Press (800ms) | Go Back / Exit current screen |
| BTN1 | Toggle Load Switch (works in ANY state) |
| BTN2 | Toggle 17V Buck (works in ANY state, only if VBUS > 18V) |

## Directory Structure

```
src/
├── main.cpp                 # Entry point, main event loop
├── hardware.h/cpp           # Hardware singleton (all drivers)
├── interrupts.h/cpp         # Centralized GPIO interrupt routing
├── config/
│   ├── board_config.h       # Pin definitions (Board:: namespace)
│   ├── app_config.h         # Timeouts, thresholds (AppConfig:: namespace)
│   └── version.h            # Firmware version (Version:: namespace)
├── drivers/                 # Low-level hardware drivers (no business logic)
│   ├── gpio/                # SimpleIO (digital I/O with non-blocking blink)
│   ├── input/               # Button, RotaryEncoder, ADCInputs
│   ├── buzzer/              # PWM-based melody playback
│   ├── rgb_led/             # SK6812 via PIO
│   ├── display/             # ST7789 LCD (SPI, 240x320)
│   └── power/
│       ├── ina228/          # Power monitor (I2C 0x40, 8mΩ shunt)
│       └── tps26750/        # USB PD controller (I2C 0x21)
├── logic/                   # Application logic
│   ├── state_machine.h/cpp  # AppState transitions, encoder/button handling
│   ├── settings.h/cpp       # User settings (current limit, PDO, output states)
│   ├── safety.h/cpp         # Safety monitoring (temp, voltage, overcurrent)
│   └── pd_manager.h/cpp     # PD contract caching and negotiation state machine
├── utils/
│   ├── logging.h            # LOG_INFO, LOG_WARN, LOG_ERROR, LOG_DEBUG, LOG_CRITICAL
│   ├── eeprom_loader.h/cpp  # TPS26750 EEPROM flashing (disabled by default)
│   └── tps26750_patch.c     # TPS26750 binary configuration
└── ui/
    ├── display_manager.h/cpp  # All screen rendering (monolithic, no separate screen files)
    └── assets/
        └── synapticon_logo.h
```

## Key Subsystems

### USB Power Delivery (TPS26750)

Negotiates voltage contracts with USB-C chargers. Supports Fixed, PPS (5-21V programmable), and AVS (15-48V EPR) profiles. Key flow:

1. `getSourceCapabilities()` → discover available contracts
2. `requestFixedProfile()` / `requestPPSProfile()` / `requestAVSProfile()` → negotiate
3. Monitor `INT_EVENT1` bit 12 (NEW_CONTRACT_AS_SINK) via interrupt flag
4. `getActiveContract()` → verify negotiated voltage/current

The `PdManager` wraps this with caching and a negotiation state machine (`IDLE` → `REQUESTING` → `SUCCESS`/`FAILED`/`TIMEOUT`).

### Power Monitoring & Safety (INA228 + Safety Module)

- INA228: Measures voltage, current, power, die temperature via I2C. Configured with 8mΩ shunt, 5A max.
- Overcurrent: Hardware ALERT pin (active-low, latched) triggers ISR that immediately disables load switch.
- `SafetyState` includes both NTC board temperature (`temperature_c`) and INA228 die temperature (`ina_temperature_c`).
- VBUS detection uses **ADC pre-switch measurement** (`hw.adc.getVBUS()`), not INA228 post-switch (which reads 0V when load switch is off).

### Display Rendering (DisplayManager)

All rendering is in `display_manager.cpp` (monolithic - no separate screen files despite the plan mentioning them). Screens: `renderBootScreen()`, `renderMainScreen()`, `renderMenuScreen()`, `renderAdjustScreen()`, `renderFaultScreen()`.

**Flicker-free rendering pattern:** Use fixed-width format strings (`%6.2f`) to overwrite previous values without clearing. Track previous values (`_last_menu_selection`, `_last_pdo_selection`, `_last_adjust_value`) and only redraw changed items. Full redraws only on state change via `_needs_full_redraw` flag.

### EEPROM Flashing (TPS26750 Config)

Disabled by default. To flash: set `ENABLE_EEPROM_FLASHING` to 1 in `src/utils/eeprom_loader.h`, rebuild, flash, power cycle TPS26750, then set back to 0.

## Important Patterns

### Non-Blocking Timing
```cpp
static absolute_time_t next_event = make_timeout_time_ms(1000);
if (absolute_time_diff_us(next_event, get_absolute_time()) >= 0) {
    // Time elapsed - execute
    next_event = make_timeout_time_ms(1000);
}
```

### Static Wrappers for C Callbacks
```cpp
static void stopToneCallback(void *param) {
    static_cast<Buzzer*>(param)->stopTone();
}
```

### Hardware Key GPIOs
- `hw.loadSwitch` (GPIO 3) - Main output enable/disable
- `hw.overcurrentAlert` (GPIO 12) - INA228 ALERT pin (active low, latched)
- `hw.EN_17V` (GPIO 20) - 17V buck enable (only when VBUS > 18V)

## Naming Conventions

- Classes: `PascalCase`
- Functions: `camelCase`
- Variables: `snake_case`
- Constants: `UPPER_SNAKE_CASE`
- Private members: `_leading_underscore`

## Mandatory Rules

1. **No global variables** outside the six singletons (`hw`, `stateMachine`, `safety`, `pdManager`, `displayManager`, `settings`)
2. **No blocking delays** in main loop
3. **No I2C/SPI in ISRs** (except overcurrent safety cutoff)
4. **Drivers are hardware-focused** - no business logic, no cross-driver dependencies, no project-specific includes (like logging)
5. **YAGNI** - implement only what's needed now
6. **Driver functions return `bool` status** for error handling
7. **Use logging macros** in application code: `LOG_INFO`, `LOG_WARN`, `LOG_ERROR`, `LOG_DEBUG`, `LOG_CRITICAL`, `LOG_HW_INIT()`

## Safety-Critical Code

**Overcurrent Protection** (in `interrupts.cpp`):
- Triggered by INA228 ALERT pin (active-low, latched)
- Immediately disables load switch in ISR (no delays)
- Sets red LED for visual indication
- Must clear latch via `getDiagnoseAlert()` before re-enable
- ISR only triggers when `PIN_SWITCH_EN` is HIGH (avoids false triggers when switch is off)

**Do not modify** without understanding electrical safety implications.

## Historical Bug Fixes

These document non-obvious gotchas. Read before modifying related code.

**Button Debouncing:** Static variable was shared across instances, causing multi-triggers. Fixed with per-instance `was_pressed_for_click` member.

**VBUS Measurement:** INA228 (post-switch) shows 0V when load switch is off. Fixed by using ADC pre-switch measurement (`hw.adc.getVBUS()`) for PD connection detection.

**Overcurrent False Triggers:** INA228 ALERT pin goes low when load switch is disabled, not just on overcurrent. Fixed by checking `gpio_get(Board::PIN_SWITCH_EN)` in ISR - only trigger if switch was supposed to be ON.

**Boot Sequence Protection:** Safety faults during BOOT state caused transition to FAULT before boot screen was visible. Fixed by skipping fault transitions while in BOOT state.

**Display Flickering:** Clearing screen areas every frame caused visible flicker. Fixed by using fixed-width format strings to overwrite previous values, and only performing full redraws on state change.

**Lesson:** Pre-switch vs post-switch measurements matter. ISR conditions must account for all GPIO states. Use overwrite-based rendering instead of clear-then-draw.

## Hardware Details

- **LCD:** 240x320 (2.4") ST7789, SPI @ 10MHz, 180° rotation (MADCTL 0xC0)
- **17V Buck:** STO/SBC voltage for motor drive safety (200mA fused), enable only when VBUS > 18V
- **ADC Voltage:** Pre-switch VBUS (150kΩ/10kΩ divider), redundant to INA228 post-switch
- **ADC Temp:** NTC thermistor (Beta=3950, 10kΩ @ 25°C, 4.7kΩ series resistor)
- **Current Resolution:** Target 1mA precision for user adjustment
- **Startup:** Output disabled by default, user enables via BTN1
- **UART:** TX=GP16, RX=GP29 @ 115200 (configured in CMakeLists.txt)
- **I2C0:** GP4/GP5 @ 400kHz (INA228 0x40, TPS26750 0x21)
- **I2C1:** GP14/GP15 @ 400kHz (CAT24C512 EEPROM 0x50, for TPS26750 config)
- **RGB LED:** SK6812 on GP28 via PIO (GRB color order)
