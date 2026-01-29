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
- PPS badge indicator when using programmable power supply mode
- Rounded rectangle frame around power readings (pink Synapticon brand color)
- Measured V, I, P from INA228 with large anti-aliased fonts
- Secondary readings: Vin, current limit
- Temperature readings (NTC board temp, INA die temp)
- Output status with inverted rounded badges:
  - Load Switch: Green "ON" / Red "OFF"
  - 17V Buck: Yellow "ON" (safety STO/SBC) / Gray "OFF"
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
- Product name (large), subtitle with Synapticon logo
- HW version, FW version, author, build date, target, max specs
- Flash usage (auto-calculated from linker symbols, shows KB and %)
- GitHub link (synapticon/PD240W in blue hyperlink color)
- Company name (Synapticon GmbH)
- Read-only: any click or long press returns to menu (with exit beep)

**Fault Screen** ✅
- Warning icon, fault type, measured/limit values
- Click to acknowledge and return to MAIN

#### 4.2 Bug Fixes & Refinements (Phase 4)
- Ghost image fix (backlight delayed until first frame renders)
- Boot text flicker (message pointer tracking)
- Contract current /10 error (RDO vs PDO field)
- Button queuing during boot (ISR flag draining)
- Badge corner artifacts (fillRect with +1px margin before fillRoundRect)
- Encoder acceleration for current limit (velocity scaling)

#### 4.3 UI Features Implemented
- Rounded rectangle primitives (`drawRoundRect`, `fillRoundRect`)
- Power readings in pink Synapticon-branded frame
- ON/OFF badges with inverted colors and rounded corners
- Menu navigation beeps (entry/exit/confirm sounds)
- Flash usage display with linker symbols
- Build date via CMake injection
- Temperature display with °C notation
- Configurable STDIO (UART/USB) in CMakeLists.txt

#### 4.4 Remaining UI Work
- Improve encoder tick counting (misses ticks during fast rotation)

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

#### 6.1 Settings Submenu (Planned)
Reorganize menu structure to group configuration options under a "Settings" submenu:

```
Main Menu:
> Select Voltage
  Current Limit
  Settings          <-- NEW submenu
  About

Settings Submenu:
> Flash EEPROM      (moved from main menu)
  PPS Calibration   (NEW feature, see 6.2)
  Back
```

**Benefits:**
- Cleaner main menu with fewer items
- Logical grouping of advanced/rarely-used options
- Room for future settings (brightness, sounds, etc.)

#### 6.2 Automatic PPS Voltage Calibration (Planned)
**Problem:** PPS chargers often output slightly different voltages than requested. When you request 12.00V, you might get 12.15V or 11.92V. This affects precision applications.

**Solution:** An automatic calibration routine that:
1. Requests multiple voltage points across the PPS range
2. Measures actual output voltage via INA228
3. Builds a calibration table (requested → actual)
4. Applies correction when user selects a target voltage

**Proposed Flow:**
```
User enters Settings > PPS Calibration

Screen 1: "PPS Calibration"
  "This will measure your charger's
   voltage accuracy at multiple points.
   Takes ~30 seconds. Load switch will
   be enabled during calibration."
   
   [Start] [Cancel]

Screen 2: Calibration in progress
  Progress bar: "Testing 5.0V..."
  Shows: Requested: 5.00V  Actual: 5.12V
  
Screen 3: Results
  "Calibration complete!
   Average offset: +0.08V
   Max error: 0.15V @ 15V
   
   [Save] [Discard]"
```

**Data Structure:**
```cpp
struct PpsCalibration {
    bool valid;
    uint8_t num_points;         // e.g., 8 points
    uint16_t requested_mv[8];   // 5000, 7000, 9000, 11000, ...
    int16_t offset_mv[8];       // +120, +80, +50, -20, ...
};
```

**Usage:** When user selects a PPS voltage, apply linear interpolation from calibration table to request a corrected value that results in the desired actual output.

**Settings Toggle:** "Enable PPS Calibration: [ON]/OFF" in Settings menu to enable/disable correction.

**Storage:** Calibration data could be stored in RP2040 flash (not EEPROM) using Pico SDK's flash storage, persisting across power cycles.

#### 6.3 Improve Encoder Tick Counting (Pending)
- Encoder still misses some ticks during fast rotation
- Investigate: ISR debounce timing, quadrature decoding edge detection
- Consider counting on both edges for 2x or 4x resolution

#### 6.4 Thermometer Temperature Widget (Optional)
- Programmatic thermometer graphic on main screen (bottom-right corner)
- Color-coded fill: blue (<30°C) → yellow → orange → red (>65°C)
- Blinking above 75°C (critical warning)
- Low priority - current dual temp display works well

#### 6.5 Completed Features (Reference)

The following Phase 6 features have been fully implemented:

| Feature | Status | Notes |
|---------|--------|-------|
| PPS Mode Support | ✅ | Voltage adjustment, keep-alive, UI badge |
| EEPROM Flash Menu | ✅ | Compare, flash, verify with progress |
| Anti-aliased Fonts | ✅ | Inter font family, 3 sizes |
| Synapticon Logo | ✅ | Boot screen + About screen |
| Rounded UI Elements | ✅ | Frames, badges with fillRoundRect |

---

## Hardware Notes

### Non-PD Charger Behavior
- When connected to a non-PD charger (no USB-PD negotiation), the device operates in USB BC1.2 mode
- **Maximum current: 3A** (limited by USB-C specification for non-PD sources)
- Main screen shows "USB 5V (no PD)" to indicate fallback mode
- PDO selection shows "No PD contracts" with helpful message

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
