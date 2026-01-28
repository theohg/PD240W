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
| **Current Sensing** | INA228 with 8mΩ shunt, 5A max, target 0.001A resolution |
| **ADC Voltage** | Pre-switch VBUS (redundant to INA228 post-switch) |
| **ADC Temp** | Onboard NTC (Beta=3950, 10kΩ @ 25°C, 4.7kΩ series) |
| **Startup** | Output disabled by default, user enables via BTN1 |
| **Overcurrent** | Load switch off, clear INA228 fault latch to recover |
| **Safety** | Overvoltage, overtemperature, power limit enforcement |
| **Startup Sound** | Mario power-up melody |

---

## Input Mapping (Prusa-Style)

| Control | Action |
|---------|--------|
| **Encoder Rotate** | Navigate menu / Adjust values |
| **Encoder Click** | Confirm / Select |
| **Encoder Long Press (800ms)** | Go Back / Exit current screen |
| **BTN1** | Toggle Load Switch (main output) - any state except BOOT |
| **BTN2** | Toggle 17V Buck - any state except BOOT (only if VBUS > 18V) |

---

## Development Phases

### Phase 1: Hardware Foundation ✅ COMPLETE

All hardware drivers implemented and tested:
- GPIO input/output with non-blocking blink
- ADC with voltage divider and NTC temperature conversion
- Encoder with ISR-based quadrature decoding and debouncing
- ST7789 LCD with complete initialization, graphics, and text rendering
- Buzzer with PWM-based melody playback
- Button debouncing (per-instance state tracking)
- INA228 power monitoring
- SK6812 RGB LED via PIO

---

### Phase 2: TPS26750 USB PD Integration ✅ COMPLETE

Full USB Power Delivery negotiation implemented:
- I2C communication with TPS26750-specific byte-count protocol
- Complete register map (29 registers)
- 4CC command support (Gaid, SWSk, GSrC, GSkC)
- Interrupt handling (read/clear/check INT_EVENT1)
- Source capability discovery (Fixed, PPS, AVS parsing)
- Voltage negotiation: `requestFixedProfile()`, `requestPPSProfile()`, `requestAVSProfile()`
- AUTONEGOTIATE_SINK register manipulation (24-byte complex bit packing)
- EPR support for 28V, 36V, 48V contracts

**Test Program:** `main.cpp` displays contracts on LCD, allows selection via encoder, and negotiates with charger.

---

### Phase 3: Application Logic & State Machine ✅ COMPLETE

**Goal:** Implement control logic and user interaction

#### 3.0 Project Reorganization ✅ COMPLETE
- Created `src/config/` with `board_config.h`, `app_config.h`, and `version.h`
- Moved EEPROM files to `src/utils/` (`eeprom_loader.h/cpp`, `tps26750_patch.c`)
- Updated CMakeLists.txt with new paths
- Updated documentation (CLAUDE.md, DEVELOPMENT_PLAN.md)

#### 3.1 Application States ✅ IMPLEMENTED
```cpp
enum class AppState {
    BOOT,      // Startup: logo, version, melody (3s)
    MAIN,      // Real-time monitoring display
    MENU,      // PDO selection / settings navigation
    ADJUST,    // Adjusting a value (voltage/current)
    FAULT      // Error display, needs acknowledgment
};
```

#### 3.2 State Transitions ✅ IMPLEMENTED
```
BOOT ──(3s timeout)──> MAIN

MAIN <──(long press)──> MENU
     <──(fault)───────> FAULT

MENU ──(select)──> ADJUST ──(confirm/back)──> MENU
     <──(long press)──> MAIN
     <──(30s timeout)──> MAIN

FAULT ──(click acknowledge)──> MAIN
```

#### 3.3 Boot Sequence (3 seconds) ✅ IMPLEMENTED
| Time | Event |
|------|-------|
| 0ms | Display Synapticon logo (centered) |
| 100ms | Show "PD240W Power Supply" |
| 200ms | Show firmware version |
| 300ms | Start Mario power-up melody |
| 500ms | Show "Reading USB-PD..." |
| 800ms | Read source capabilities |
| 1500ms | Show contract summary |
| 3000ms | Transition to MAIN state |

#### 3.4 Settings Manager ✅ IMPLEMENTED
- Target voltage (from available contracts)
- Current limit (100mA - 5000mA, 100mA steps)
- 17V buck enable/disable
- Output enable/disable

#### 3.5 Safety Logic ✅ IMPLEMENTED
| Condition | Warning | Fault | Action |
|-----------|---------|-------|--------|
| Overcurrent | - | INA228 ALERT | ISR disables load immediately |
| Temperature | 60C | 80C | Disable load, show fault |
| PD Disconnect | - | No VBUS | Disable load, show fault |

**Implementation Notes:**
- VBUS measured via ADC pre-switch (not INA228 post-switch) for reliable PD detection
- Overcurrent ISR only triggers when load switch is enabled (PIN_SWITCH_EN high)
- Safety faults skipped during BOOT state to allow boot sequence to complete

#### 3.6 Files Created ✅ COMPLETE
- `src/logic/state_machine.h/cpp` - Main state controller
- `src/logic/settings.h/cpp` - User settings management
- `src/logic/safety.h/cpp` - Safety monitoring
- `src/logic/pd_manager.h/cpp` - PD contract management
- `src/ui/display_manager.h/cpp` - State-based screen rendering

---

### Phase 4: User Interface 🔄 IN PROGRESS

**Goal:** Create intuitive LCD menu and displays

#### 4.1 Implemented Screens

**Boot Screen** ✅
- Synapticon logo (centered)
- Product name, firmware version, boot stage messages
- Progress bar with stage tracking
- Ghost image fix (backlight delayed until framebuffer cleared)
- Boot text flicker fix (only redraws on message change)
- BTN1/BTN2 disabled during boot (ISR flags drained on BOOT→MAIN transition)

**Main Screen** ✅
- Active contract display (voltage @ current, or "USB 5V (no PD)" for non-PD chargers)
- Measured V, I, P from INA228
- Temperature readings
- Output status (Load: ON/OFF, 17V: ON/OFF)
- Flicker-free rendering with fixed-width format strings

**Menu Screen** ✅
```
> Select Voltage      (enter PDO list)
  Current Limit       (enter adjustment)
  About               (show device info)
```

**PDO Selection** ✅
- Scrollable list with highlighted selection
- Shows "No PD contracts" with helpful message for non-PD chargers
- Supports up to 13 PDOs with scroll window

**Current Limit Adjustment** ✅
- Large value display with progress bar
- Encoder acceleration (step multiplied by accumulated ticks)
- Capped to active contract max via `getEffectiveMaxCurrentMa()`
- Min/max labels with "(max)" indicator

**About Screen** ✅
- Product name (large), subtitle
- HW version, FW version, author, build date, target, max specs
- Company name (Synapticon GmbH)
- Read-only: any click or long press returns to menu

**Fault Screen** ✅
- Warning icon, fault type, measured/limit values
- Click to acknowledge and return to MAIN

#### 4.2 Bug Fixes During Phase 4
- LCD ghost image (backlight timing in `st7789.cpp`)
- Boot text flicker (message pointer tracking)
- Contract current /10 error (RDO vs PDO field in `tps26750.cpp`)
- Button queuing during boot (ISR flag draining)
- Button polling reliability (switched to ISR-based detection)
- Encoder acceleration for current limit adjustment

#### 4.3 Files
- `src/ui/display_manager.h/cpp` - All screen rendering (monolithic, no separate screen files)

#### 4.4 Remaining UI Work
- Display real Synapticon logo during boot (220x220 RGB565 bitmap from `src/ui/assets/synapticon_logo.h`) and small version in About screen
- Improve encoder tick counting (still misses ticks during fast rotation)
- Thermometer temperature widget on main screen (bottom-right, color-coded, see Phase 6.3 for details)
- Better font rendering for large text (current 5x7 bitmap looks pixelated at large sizes)
- UI visual improvements: oval/rounded bounding boxes around voltage, current, power readouts (details TBD)

---

### Phase 5: Final Integration & Testing (Pending)

**Goal:** Complete system testing and optimization

#### 5.1 End-to-End Testing
- Complete workflow: boot → select voltage → enable output → adjust current
- Safety features: trigger overcurrent → observe shutdown → recover
- Temperature monitoring and protection
- Multiple USB-C chargers (different capabilities)

#### 5.2 Edge Cases
- Unplug charger while output on
- Rapid setting changes
- Fast encoder navigation
- Boundary values (0V, 48V, 0A, 5A)

#### 5.3 Optimization
- LCD refresh rate
- Reduce unnecessary I2C transactions
- Power monitoring update frequency

#### 5.4 Code Cleanup
- Remove/wrap debug printf
- Comments on complex sections
- Resolve all TODOs
- Update documentation

---

### Phase 6: Enhancements & Future Features (Planned)

**Goal:** UI polish, new features, and advanced PD support

#### 6.1 Display Synapticon Logo
- Render the 220x220 RGB565 bitmap from `src/ui/assets/synapticon_logo.h` during boot splash (full size, centered)
- Display a scaled-down version in the About screen
- The bitmap is pre-encoded in ST7789 RGB565 format, ready for direct SPI transfer

#### 6.2 Improve Encoder Tick Counting
- Encoder still misses some ticks during fast rotation
- Investigate: ISR debounce timing (currently 1ms), quadrature decoding edge detection, hardware filtering
- Consider counting on both edges (A and B channels) for 2x or 4x resolution

#### 6.3 Thermometer Temperature Widget
- Programmatic thermometer graphic on main screen (bottom-right corner)
- Vertical bar with rounded bulb at bottom, ~12px wide, ~80px tall
- Color-coded fill levels: blue (<30°C), yellow (30-50°C), orange (50-65°C), red (65-80°C)
- Blinking effect above 75°C (critical temperature warning)
- INA die temp and NTC board temp labels displayed to the left of the thermometer
- Implement as a standalone `drawThermometer()` function so it can be easily removed if it doesn't look good

#### 6.4 Better Font Rendering
- Current 5x7 bitmap font looks pixelated at large sizes (especially main screen readouts)
- Options to evaluate:
  - Larger bitmap font (8x16 or custom designed)
  - Anti-aliased font rendering (grayscale pixels for smoother edges)
  - Pre-rendered digit sprites for the big voltage/current/power values
- Priority: main screen large numbers, About screen title

#### 6.5 PPS Mode Support ✅ COMPLETE
Programmable Power Supply mode allows fine-grained voltage control within a range.

- **6.5a** Parse PPS capabilities from source caps (min/max voltage, max current) ✅ — already implemented in `SourceCapability` struct (`is_pps`, `min_voltage_mv`)
- **6.5b** Add PPS-aware PDO selection UI ✅ — PDO list shows "PPS x-yV zzzzmA" for PPS profiles, clearly indicating programmable range
- **6.5c** Implement voltage adjustment within PPS range ✅ — new `AdjustMode::PPS_VOLTAGE` mode where encoder adjusts millivolts (20mV steps per PD spec). Shows progress bar, min/max labels, and current selection
- **6.5d** Call `requestPPSProfile()` with user-selected voltage ✅ — integrated via `pd_manager.requestPpsVoltage()`
- **6.5e** Implement PPS keep-alive ✅ — `PdManager::update()` automatically refreshes PPS contract every 7 seconds (under 10-second spec limit). Tracks `_pps_active`, `_pps_voltage_mv`, `_pps_current_ma`, `_pps_last_refresh`
- **6.5f** Update main screen for PPS ✅ — shows green "PPS" badge next to contract info when PPS mode is active

**Implementation Details:**
- State machine tracks PPS state: target voltage, min/max range, max current, PDO index
- When user selects PPS PDO from list, enters voltage adjustment mode instead of immediate request
- Display shows "Programmable Power" header with large voltage display, progress bar, and range labels
- PPS keep-alive runs in `pd_manager.update()`, transparent to application
- PPS state is deactivated when switching to fixed/AVS profiles

#### 6.6 EEPROM Flash Menu Item ✅ COMPLETE
Add "Flash EEPROM" entry to the settings menu for runtime TPS26750 configuration updates.

- **UI Flow:** ✅
  1. User selects "Flash EEPROM" from menu
  2. Firmware reads EEPROM content and compares against `tps26750_patch.c` binary
  3. Display comparison result:
     - "Config identical" — EEPROM already has the same binary
     - "EEPROM empty" — no valid data found
     - "Different config found" — EEPROM has a different configuration
  4. Prompt: "Proceed with flash?" with Yes/No selection via encoder (if not identical)
  5. On Yes: flash with progress bar, verify, show result (success/failure)
  6. On No: return to menu

- **Implementation Notes:** ✅
  - Existing functions in `eeprom_loader.cpp`: `eeprom_write_block()`, `eeprom_read_block()`, `eeprom_already_programmed()`, `flashTps26750Eeprom()`
  - Refactored: separate the compare and flash steps into individual callable functions (now accessible at runtime)
  - Properly inits/deinits I2C1 for EEPROM access
  - Flash is a blocking operation (~5-10 seconds) — display progress updates via callback
  - After successful flash, instructs user to power cycle the TPS26750

#### 6.7 UI Visual Improvements (Pending)
- Redesign main screen power readouts with oval/rounded bounding boxes around voltage, current, and power values
- Goal: visually appealing, clear separation of metrics
- Specific design TBD — to be discussed in detail before implementation
- May involve: rounded rectangle drawing primitive, layout redesign, color scheme refinement

---

## File Structure (Current)

```
src/
├── main.cpp                 # Entry point, main event loop
├── hardware.h/cpp           # Global Hardware singleton
├── interrupts.h/cpp         # GPIO interrupt handling
├── config/                  # ✅ All configuration files
│   ├── board_config.h       # Pin definitions (Board:: namespace)
│   ├── app_config.h         # Timeouts, thresholds, constants
│   └── version.h            # Version, author, company (Version:: namespace)
├── drivers/                 # ✅ Complete - all hardware drivers
│   ├── gpio/
│   ├── input/
│   ├── buzzer/
│   ├── rgb_led/
│   ├── display/
│   └── power/
│       ├── ina228/
│       └── tps26750/
├── logic/                   # ✅ Phase 3 - complete
│   ├── state_machine.h/cpp  # ✅ Main state controller (incl. AdjustMode::ABOUT)
│   ├── settings.h/cpp       # ✅ User settings management
│   ├── safety.h/cpp         # ✅ Safety monitoring
│   └── pd_manager.h/cpp     # ✅ PD contract management
├── utils/
│   ├── logging.h            # LOG_INFO, LOG_ERROR, etc.
│   ├── eeprom_loader.h/cpp  # ✅ TPS26750 EEPROM flashing
│   └── tps26750_patch.c     # ✅ TPS26750 configuration binary
└── ui/                      # 🔄 Phase 4 - core screens done, refinement ongoing
    ├── display_manager.h/cpp # ✅ All screen rendering (monolithic)
    └── assets/
        └── synapticon_logo.h # 220x220 RGB565 bitmap
```

---

## Code Patterns to Use

### Keep These Patterns ✅
- Hardware Singleton (`extern Hardware hw`)
- Namespace Organization (`Board::`, `AppConfig::`, `Version::`)
- Separate driver layer
- Non-blocking timers (`absolute_time_t`)
- ISR-based button detection (not polling)
- Flicker-free rendering (overwrite, don't clear-then-draw)

### Patterns for New Code
- **State Machine:** Simple enum with switch/case (no class inheritance)
- **Error Handling:** `bool` or `ErrorCode` returns
- **Logging:** Use `LOG_*` macros in application code, not in drivers
- **Configuration:** Use `AppConfig::` namespace for app constants

---

## Testing Strategy

| Phase | Testing Approach |
|-------|-----------------|
| Phase 1 | ✅ Each driver tested independently |
| Phase 2 | ✅ TPS26750 tested with USB-C charger, verified with INA228 |
| Phase 3 | ✅ State transitions tested, safety bugs fixed |
| Phase 4 | 🔄 Menu navigation, boot sequence, fault display tested; UI refinement ongoing |
| Phase 5 | Full integration testing |
| Phase 6 | Per-feature testing as implemented |

---

## Notes

- This plan is flexible - adjust as requirements evolve
- Phases can partially overlap
- Testing should happen continuously
- Safety features must be tested with real fault conditions
- Prusa-style input mapping provides intuitive single-control navigation
- Phase 6 features are independent and can be implemented in any order
