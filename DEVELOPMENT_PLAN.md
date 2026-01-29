# PD240W Development Plan

## Project Goal

Create an adjustable power supply (0-48V, 0-5A, up to 240W) using USB-C Power Delivery negotiation. The user navigates a menu on an LCD screen using a Prusa-style rotary encoder:
1. Select voltage from available PD contracts
2. Set current limit (monitored by INA228)
3. Enable optional 17V buck converter

---

## Hardware Specifications

| Component | Specification |
|-----------|--------------|
| **LCD** | 240x320 (2.4") ST7789, model HS20HS072RX |
| **17V Buck** | STO/SBC voltage for motor drives, 200mA fused, enable when VBUS > 18V |
| **USB-C PD** | TPS26750 supports all contracts up to 48V including PPS and AVS |
| **Current Sensing** | INA228 with 8mΩ shunt, 5A max (3A for non-PD), 0.001A resolution |
| **ADC Voltage** | Pre-switch VBUS (redundant to INA228 post-switch) |
| **ADC Temp** | Onboard NTC (Beta=3950, 10kΩ @ 25°C, 4.7kΩ series) |
| **Startup** | Output disabled by default, user enables via BTN1 |
| **Overcurrent** | Load switch off, clear INA228 fault latch to recover |
| **Safety** | Overvoltage, overtemperature, power limit enforcement |

---

## Input Mapping (Prusa-Style)

| Control | Action |
|---------|--------|
| **Encoder Rotate** | Navigate menu / Adjust values |
| **Encoder Click** | Confirm / Select |
| **Encoder Long Press (700ms)** | Go Back / Exit current screen |
| **BTN1** | Toggle Load Switch (main output) - any state except BOOT |
| **BTN2** | Toggle 17V Buck - any state except BOOT (only if VBUS > 18V) |

---

## Development Phases

### Phase 1: Hardware Foundation ✅ COMPLETE

All hardware drivers implemented and tested:
- GPIO, ADC, Encoder, ST7789 LCD, Buzzer, Button, INA228, SK6812 RGB LED

---

### Phase 2: TPS26750 USB PD Integration ✅ COMPLETE

Full USB Power Delivery negotiation:
- I2C communication, register map, 4CC commands, interrupt handling
- Source capability discovery (Fixed, PPS, AVS)
- Voltage negotiation with EPR support (28V, 36V, 48V)

---

### Phase 3: Application Logic & State Machine ✅ COMPLETE

- **States:** BOOT → MAIN ↔ MENU → ADJUST, FAULT
- **Settings:** Target voltage, current limit (10mA-5A), output enables
- **Safety:** Overcurrent ISR, temperature monitoring, PD disconnect detection

---

### Phase 4: User Interface ✅ COMPLETE

**Implemented Screens:**
- **Boot:** Synapticon logo, version, progress bar, Mario melody
- **Main:** Contract display, power readings (V/I/P), temperatures, status badges
- **Menu:** Select Voltage, Current Limit, Flash EEPROM, About
- **PDO Selection:** Scrollable list with PPS/AVS support
- **Current Limit:** Large display with progress bar, encoder acceleration
- **About:** HW/FW version, flash usage, GitHub link
- **Fault:** Warning display with click-to-acknowledge

**UI Features:**
- Anti-aliased fonts (Inter family, 3 sizes)
- Rounded rectangles and inverted badges
- Synapticon pink branding
- Menu navigation beeps
- °C temperature notation
- Configurable STDIO (UART/USB)

---

### Phase 5: Enhancements & Future Features 🔄 IN PROGRESS

#### 5.1 Settings Submenu ✅ COMPLETE
Reorganized menu to group advanced options:
```
Main Menu:
> Select Voltage
  Current Limit
  Settings          <-- NEW submenu
  About
  Back              <-- Returns to main screen

Settings Submenu:
> Flash EEPROM
  Auto PPS tuning   [ON/OFF toggle]
  Brightness        [5-100% value, click to adjust]
  Sounds            [ON/OFF toggle]
  Back
```

**Features:**
- All settings persisted to RP2040 flash (survives power cycle)
- Navigation buzzer sounds (respects Sounds setting)
- Brightness adjustment with PWM backlight control (perceptual curve)
- Toggle items (Auto PPS, Sounds) switch on single click
- Brightness enters adjustment mode on click, knob adjusts 5-100%
- CW rotation decreases brightness (matches menu down direction)
- Long press functionality removed (kept as variable for future use)

---

### Phase 6: Planned Features 🔮 FUTURE

#### 6.1 Automatic PPS Voltage Calibration
**Problem:** PPS chargers output slightly different voltages than requested.

**Solution:** Calibration routine that:
1. Requests multiple voltage points across PPS range
2. Measures actual output via INA228
3. Builds correction table (requested → actual)
4. Applies interpolation for precise output

**Data Structure:**
```cpp
struct PpsCalibration {
    bool valid;
    uint8_t num_points;
    uint16_t requested_mv[8];
    int16_t offset_mv[8];
};
```

**Storage:** RP2040 flash for persistence across power cycles.

#### 6.2 USB Serial CLI
Command-line interface via USB CDC for:
- Remote monitoring (voltage, current, power, temperature)
- Configuration without display
- Firmware diagnostics and debugging
- Scripted testing

#### 6.3 Power Logging / Data Export
- Log power readings to internal buffer
- Export via USB serial as CSV
- Useful for load profiling and debugging

#### 6.4 Charger Compatibility Database
- Store known charger characteristics
- Auto-detect and display charger model
- Report compatibility issues

#### 6.5 Custom Voltage Presets
- Save user-defined voltage+current combinations
- Quick recall from menu
- Useful for repetitive testing

#### 6.6 Power-On Behavior Settings
- Option to auto-enable output on boot
- Remember last voltage setting
- Configurable boot delay

#### 6.7 Screen Saver / Auto-Dim
- Dim or blank display after inactivity
- Wake on encoder movement
- Reduces power consumption

#### 6.8 Multi-Language Support
- String table abstraction
- Language selection in settings

---

### Completed Features Summary

| Feature | Status |
|---------|--------|
| PPS Mode Support | ✅ |
| EEPROM Flash Menu | ✅ |
| Anti-aliased Fonts | ✅ |
| Synapticon Logo | ✅ |
| Rounded UI Elements | ✅ |
| Settings Submenu | ✅ |
| Brightness Control (PWM) | ✅ |
| Sound Mute Option | ✅ |
| Flash Persistence | ✅ |

---

## Hardware Notes

### Non-PD Charger Behavior
- USB BC1.2 mode when no PD negotiation
- **Maximum current: 3A** (enforced in `getEffectiveMaxCurrentMa()`)
- Main screen shows "USB 5V (no PD)"
- PDO selection shows "No PD contracts"

---

## File Structure

```
src/
├── main.cpp                 # Entry point, main event loop
├── hardware.h/cpp           # Hardware singleton
├── interrupts.h/cpp         # GPIO interrupt handling
├── config/                  # Configuration
│   ├── board_config.h       # Pin definitions
│   ├── app_config.h         # Constants & thresholds
│   └── version.h            # Version info
├── drivers/                 # Hardware drivers
│   ├── gpio/, input/, buzzer/, rgb_led/, display/
│   └── power/ (ina228/, tps26750/)
├── logic/                   # Application logic
│   ├── state_machine.h/cpp
│   ├── settings.h/cpp
│   ├── safety.h/cpp
│   └── pd_manager.h/cpp
├── utils/                   # Utilities
│   ├── logging.h
│   ├── eeprom_loader.h/cpp
│   └── tps26750_patch.c
└── ui/                      # User interface
    ├── display_manager.h/cpp
    └── assets/synapticon_logo.h
```

---

## Code Patterns

- Hardware Singleton (`extern Hardware hw`)
- Namespace Organization (`Board::`, `AppConfig::`, `Version::`)
- Non-blocking timers (`absolute_time_t`)
- ISR-based button/encoder detection
- Flicker-free rendering (overwrite, don't clear-then-draw)
- Logging via `LOG_*` macros

---

## Notes

- Plan is flexible - adjust as requirements evolve
- Testing happens continuously with real hardware
- Safety features tested with actual fault conditions
