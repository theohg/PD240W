# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**PD240W** is an adjustable power supply for motor drives using USB-C Power Delivery negotiation, supporting up to 240W at 48V 5A. Firmware runs on a Raspberry Pi Pico (RP2040).

- **Target:** RP2040 (Raspberry Pi Pico), C++17, Pico SDK v2.2.0
- **Build:** CMake + Ninja
- **Status:** Phase 5 (Settings submenu) complete. See [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md) for roadmap.
- **Key features:** Voltage selection from PD contracts (up to 48V), adjustable current limiting (10mA-5A via INA228), LCD menu with Prusa-style encoder navigation, overcurrent/overtemperature protection, optional 17V buck converter, settings persistence to flash
- **Non-PD fallback:** 5V @ 3A max when connected to non-PD chargers (USB BC1.2 mode)

## Build Commands

### Quick Reference

| Command | What it does |
|---------|-------------|
| `ninja` | Fast incremental build (only changed files) |
| `cmake .. && ninja` | Reconfigure + build (updates build date) |
| `rm -rf * && cmake -G Ninja .. && ninja` | Full clean rebuild |

### Detailed Build Commands

```bash
# INCREMENTAL BUILD (fastest, use during development)
# Only recompiles files that changed. Does NOT update build date.
cd build && ninja

# RECONFIGURE + BUILD (use when CMakeLists.txt changes or to update build date)
# Regenerates build system, then compiles. Updates BUILD_DATE in About screen.
cd build && cmake -G Ninja .. && ninja

# CLEAN BUILD (use when things are broken or for releases)
# Deletes everything in build/, regenerates from scratch.
cd build && rm -rf ./* && cmake -G Ninja .. && ninja

# FLASH via USB (no debugger needed)
# Hold BOOTSEL button while connecting USB, drag .uf2 to mounted drive
cp build/PD240W.uf2 /Volumes/RPI-RP2/

# FLASH via debug probe (SWD)
# Uses OpenOCD with CMSIS-DAP probe. VSCode task "Flash" does this.
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
  -c "adapter speed 5000; program build/PD240W.elf verify reset exit"

# SERIAL DEBUGGING (UART on GP16 TX, GP29 RX @ 115200 baud)
screen /dev/tty.usbserial-* 115200
# or
minicom -D /dev/tty.usbserial-* -b 115200
```

### Build Outputs

| File | Purpose |
|------|--------|
| `build/PD240W.uf2` | Flash image for USB drag-and-drop |
| `build/PD240W.elf` | Debug binary with symbols (for SWD flash) |
| `build/PD240W.bin` | Raw binary image |
| `build/compile_commands.json` | IDE support (clangd, VSCode) |

### Build Info

- **Build date:** Updated when running `cmake ..` (stored as `BUILD_DATE` macro)
- **Flash size:** Displayed at end of build and in About screen (uses linker symbols)
- **Post-build:** Automatically shows flash/RAM usage via `arm-none-eabi-size`

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
BOOT ──(2s timeout)──> MAIN
MAIN <──(long press)──> MENU     MAIN <──(fault)──> FAULT
MENU ──(select)──> ADJUST ──(confirm/back)──> MENU
FAULT ──(click acknowledge)──> MAIN
```

States: `BOOT`, `MAIN`, `MENU`, `ADJUST`, `FAULT` (see `AppState` enum in `state_machine.h`)

Adjust modes: `PDO_SELECT`, `CURRENT_LIMIT`, `ABOUT` (see `AdjustMode` enum). The About screen is read-only — any click or long press returns to menu.

**BTN1/BTN2 are disabled during BOOT state** — output stays off until boot completes. ISR flags are drained on BOOT→MAIN transition to prevent queued presses from firing.

**Menu timeout:** Auto-returns to MAIN after 10 seconds of inactivity in MENU state.

**Encoder acceleration:** In current limit adjustment, the step size is multiplied by the number of accumulated encoder ticks (`_encoder_delta`), allowing fast turns to make larger jumps.

### Input Mapping (Prusa-Style)

| Control | Action |
|---------|--------|
| Encoder Rotate | Navigate menu / Adjust values |
| Encoder Click | Confirm / Select |
| Encoder Long Press (700ms) | Go Back / Exit current screen |
| BTN1 | Toggle Load Switch (any state except BOOT) |
| BTN2 | Toggle 17V Buck (any state except BOOT, only if VBUS > 18V) |

## Directory Structure

```
src/
├── main.cpp                 # Entry point, main event loop
├── hardware.h/cpp           # Hardware singleton (all drivers)
├── interrupts.h/cpp         # Centralized GPIO interrupt routing
├── config/
│   ├── board_config.h       # Pin definitions (Board:: namespace)
│   ├── app_config.h         # Timeouts, thresholds (AppConfig:: namespace)
│   └── version.h            # Version, author, company (Version:: namespace)
├── drivers/                 # Low-level hardware drivers (no business logic)
│   ├── gpio/                # SimpleIO (digital I/O with non-blocking blink)
│   ├── input/               # Button, RotaryEncoder, ADCInputs
│   ├── buzzer/              # PWM-based melody playback
│   ├── rgb_led/             # SK6812 via PIO
│   ├── display/             # ST7789 LCD (SPI, 240x320), AA font renderer & generated fonts
│   └── power/
│       ├── ina228/          # Power monitor (I2C 0x40, 8mΩ shunt)
│       └── tps26750/        # USB PD controller (I2C 0x21)
├── logic/                   # Application logic
│   ├── state_machine.h/cpp  # AppState transitions, encoder/button handling
│   ├── settings.h/cpp       # User settings with flash persistence (brightness, sounds, auto_pps)
│   ├── safety.h/cpp         # Safety monitoring (temp, voltage, overcurrent)
│   └── pd_manager.h/cpp     # PD contract caching and negotiation state machine
├── utils/
│   ├── logging.h            # LOG_INFO, LOG_WARN, LOG_ERROR, LOG_DEBUG, LOG_CRITICAL
│   ├── eeprom_loader.h/cpp  # TPS26750 EEPROM flashing (disabled by default)
│   └── tps26750_patch.c     # TPS26750 binary configuration
└── ui/
    ├── display_manager.h/cpp  # All screen rendering (monolithic, no separate screen files)
    ├── font_config.h          # Central font size configuration (FONT_LARGE/MEDIUM/SMALL)
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

#### PPS Mode (Programmable Power Supply)

PPS allows fine-grained voltage adjustment within a charger's advertised range (typically 3.3-21V). Implementation details:

- **Voltage resolution:** 20mV steps per USB PD spec
- **Keep-alive required:** PD spec mandates re-requesting the PPS contract every <10 seconds or the source reverts to 5V. Implemented in `PdManager::update()` with 7-second refresh interval.
- **State tracking:** `PdManager` tracks `_pps_active`, `_pps_voltage_mv`, `_pps_current_ma`, `_pps_last_refresh` for automatic keep-alive.
- **UI flow:** When user selects a PPS PDO, enters `AdjustMode::PPS_VOLTAGE` for voltage adjustment (encoder rotates through range). Confirm with click to apply.
- **Main screen indicator:** Rounded green "PPS" badge shown next to contract info when PPS mode is active.
- **State deactivation:** PPS state is automatically cleared when switching to Fixed or AVS profiles.

### Settings Persistence (Flash Storage)

User settings are stored in the last 4KB sector of RP2040 flash (offset 0x1FF000 for 2MB flash):

```cpp
struct SettingsData {
    uint32_t magic;           // 0x50443234 ("PD24")
    uint8_t lcd_brightness;   // 5-100%
    bool sounds_enabled;
    bool auto_pps_enabled;
    uint32_t crc32;
};
```

- **Persistence:** Survives power cycle, loaded on boot
- **Defaults:** 100% brightness, sounds enabled, auto PPS disabled
- **Flash wear:** Settings saved immediately on change (consider debouncing for production)

### Backlight Brightness (PWM)

LCD backlight uses PWM on GPIO24 with perceptual brightness curve:
- **Range:** 5-100% (minimum prevents completely dark screen)
- **Curve:** Piecewise linear approximation for perceptually linear brightness
- **PWM resolution:** 8-bit (0-255)
- **Auto-dim:** Screen dims to 5% after 1 minute of inactivity, restored on any input
- **Configuration:** `AUTO_DIM_TIMEOUT_MS` and `LCD_BRIGHTNESS_DIM` in `app_config.h`

### Power Monitoring & Safety (INA228 + Safety Module)

- INA228: Measures voltage, current, power, die temperature via I2C. Configured with 8mΩ shunt, 5A max.
- Overcurrent: Hardware ALERT pin (active-low, latched) triggers ISR that immediately disables load switch.
- `SafetyState` includes both NTC board temperature (`temperature_c`) and INA228 die temperature (`ina_temperature_c`). The max of both is used for threshold checks.
- VBUS detection uses **ADC pre-switch measurement** (`hw.adc.getVBUS()`), not INA228 post-switch (which reads 0V when load switch is off).
- **Temperature thresholds** (with 2°C hysteresis): Caution ≥50°C (yellow LED), Warning ≥65°C (orange LED), Critical ≥75°C (buzzer alarm), Shutdown/Fault ≥80°C (load disabled, red LED).
- `SafetyStatus` enum: `OK`, `CAUTION`, `WARNING`, `FAULT` — drives both RGB LED color and state machine fault transitions.

### Display Rendering (DisplayManager)

All rendering is in `display_manager.cpp` (monolithic - no separate screen files). Screens: `renderBootScreen()`, `renderMainScreen()`, `renderMenuScreen()`, `renderAdjustScreen()`, `renderFaultScreen()`.

**Flicker-free rendering pattern:** Use fixed-width format strings (`%6.2f`) to overwrite previous values without clearing. Track previous values (`_last_menu_selection`, `_last_pdo_selection`, `_last_adjust_value`, `_last_boot_message`) and only redraw changed items. Full redraws only on state change via `_needs_full_redraw` flag.

**Non-PD charger handling:** When no PD contract is active (0V@0A), the main screen shows "USB 5V (no PD)" and the PDO list shows an informational "No PD contracts" message.

**Current limit capping:** The adjustment screen uses `getEffectiveMaxCurrentMa()` to cap the user's current limit to the active contract's maximum current (or hardware max if no contract).

**About screen:** Renders product name, HW/FW version, author, build date, target, max specs, and company name from `version.h` constants.

### Anti-Aliased Font System

Text rendering uses 4-bit alpha anti-aliased bitmap fonts generated from Inter (open-source, SIL license). Three font sizes are defined in `src/ui/font_config.h`:

| Variable | Font | Size | Used For |
|----------|------|------|----------|
| `FONT_LARGE` | Inter Bold | 28px | Main screen power values (V, A, W) |
| `FONT_MEDIUM` | Inter SemiBold | 20px | Headers, contract info, adjustment values |
| `FONT_SMALL` | Inter Regular | 14px | Labels, menus, hints, secondary text |

To change a font size globally, edit `font_config.h` to point to a different generated font header.

**Font generation** (requires Python + Pillow in conda env `IAPR`):

```bash
cd tools

# Large: Inter Bold 28px (digits + units only)
/Users/theoh/anaconda3/envs/IAPR/bin/python generate_font.py \
    fonts/Inter-Bold.ttf 28 ../src/drivers/display/font_inter_28b.h \
    --chars digits --name font_inter_28b

# Medium: Inter SemiBold 20px (full ASCII)
/Users/theoh/anaconda3/envs/IAPR/bin/python generate_font.py \
    fonts/Inter-SemiBold.ttf 20 ../src/drivers/display/font_inter_20sb.h \
    --name font_inter_20sb

# Small: Inter Regular 14px (full ASCII)
/Users/theoh/anaconda3/envs/IAPR/bin/python generate_font.py \
    fonts/Inter-Regular.ttf 14 ../src/drivers/display/font_inter_14.h \
    --name font_inter_14
```

**Key files:**
- `tools/generate_font.py` — TTF → C header converter (4-bit alpha packed bitmaps)
- `tools/fonts/` — Inter TTF source files
- `src/drivers/display/aa_font.h` — `AAGlyph`/`AAFont` structs
- `src/drivers/display/font_inter_*.h` — Generated font data headers
- `src/ui/font_config.h` — Central font size configuration (`FONT_LARGE`/`FONT_MEDIUM`/`FONT_SMALL`)

**Rendering:** `ST7789::drawCharAA()` renders each character's full advance rectangle in a single SPI burst (background + alpha-blended glyph), eliminating flicker. `ST7789::drawStringAA()` and `getStringWidthAA()` handle proportional string rendering and width calculation.

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

**LCD Ghost Image:** ST7789 VRAM persists across MCU reset, so old framebuffer content was visible as a ghost image during init. Fixed by starting backlight OFF in `st7789.cpp`, enabling only after `fillScreen(BLACK)` completes.

**Boot Text Flicker:** "Reading USB-PD..." stage message was redrawn every frame during boot. Fixed by tracking `_last_boot_message` pointer in DisplayManager and only redrawing when the message changes.

**Contract Current /10 Error:** `getActiveContract()` in `tps26750.cpp` read RDO operating current (bits 19:10, auto-negotiated by TPS26750, often 1/10 of max) instead of PDO max current (bits 9:0). Fixed by reading PDO max current field.

**Button Presses During Boot:** ISR button flags persisted through the BOOT→MAIN state transition, causing queued button actions to fire immediately. Fixed by draining `checkBtn1Clicked()`/`checkBtn2Clicked()` flags on transition in `state_machine.cpp`.

**Button Polling vs ISR:** `isClicked()` polling in the main loop missed short button presses between slow loop iterations. Fixed by switching to ISR-based detection via `Interrupts::checkBtn1Clicked()`/`checkBtn2Clicked()` which capture presses via interrupt flags.

**Lessons:** Pre-switch vs post-switch measurements matter. ISR conditions must account for all GPIO states. Use overwrite-based rendering instead of clear-then-draw. RDO operating current ≠ PDO max current — always read the PDO for advertised limits. ISR flags must be drained on state transitions to prevent stale events. Prefer ISR-based button detection over polling for responsiveness. Use member variables instead of static locals for UI state that must reset on MCU warm reset.

**PPS Badge After Reset:** Static local variable for PPS badge state survived warm MCU reset, preventing badge from being drawn even when contract.is_pps was true. Fixed by using member variable `_last_pps_state` initialized to -1 and explicitly reset in `DisplayManager::init()`.

**Brightness Curve:** Initial linear PWM mapping caused perceptually non-linear brightness (harsh drop at low values). Fixed by using piecewise linear curve that maps 5-100% to more perceptually uniform brightness levels.

## Hardware Details

- **LCD:** 240x320 (2.4") ST7789, SPI @ 10MHz, 180° rotation (MADCTL 0xC0), PWM backlight on GP24
- **17V Buck:** STO/SBC voltage for motor drive safety (200mA fused), enable only when VBUS > 18V
- **ADC Voltage:** Pre-switch VBUS (150kΩ/10kΩ divider), redundant to INA228 post-switch
- **ADC Temp:** NTC thermistor (Beta=3950, 10kΩ @ 25°C, 4.7kΩ series resistor)
- **Current Limit:** 50mA–5000mA range, 50mA steps (see `AppConfig::CURRENT_LIMIT_*`)
- **Startup:** Output disabled by default, user enables via BTN1
- **UART:** TX=GP16, RX=GP29 @ 115200 (configured in CMakeLists.txt)
- **I2C0:** GP4/GP5 @ 400kHz (INA228 0x40, TPS26750 0x21)
- **I2C1:** GP14/GP15 @ 400kHz (CAT24C512 EEPROM 0x50, for TPS26750 config)
- **RGB LED:** SK6812 on GP28 via PIO (GRB color order)
