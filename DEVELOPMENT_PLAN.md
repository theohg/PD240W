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
| **BTN1** | Toggle Load Switch (main output) - works in ANY state |
| **BTN2** | Toggle 17V Buck - works in ANY state (only if VBUS > 18V) |

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

#### 4.1 Screen Layouts

**Main Screen (MAIN state):**
- Active contract (voltage @ current)
- Measured V, I, P from INA228
- Temperature
- Output status (Load: ON/OFF, 17V: ON/OFF)

**Menu Screen (MENU state):**
```
> Select Voltage      (enter PDO list)
  Current Limit       (enter adjustment)
  About               (show version, uptime)
```

**PDO Selection (ADJUST state):**
```
  5V @ 3000mA
> 9V @ 3000mA        (highlighted)
  15V @ 3000mA
  20V @ 5000mA
  [PPS] 3.3-21V
```

**Current Limit (ADJUST state):**
```
Current Limit: 2.50 A
       [====----]
       Min      Max
```

**Fault Screen:**
```
     ⚠ OVERCURRENT ⚠

   Measured: 5.23A
   Limit: 5.00A

   Load switch disabled

   [Click to acknowledge]
```

#### 4.2 Files to Create
- `src/ui/display_manager.h/cpp` - Screen rendering coordinator
- `src/ui/screens/screen_boot.h/cpp` - Boot splash screen
- `src/ui/screens/screen_main.h/cpp` - Real-time monitoring
- `src/ui/screens/screen_menu.h/cpp` - Menu navigation
- `src/ui/screens/screen_fault.h/cpp` - Fault display

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

## File Structure (Current)

```
src/
├── main.cpp                 # Entry point, state machine driver
├── hardware.h/cpp           # Global Hardware singleton
├── interrupts.h/cpp         # GPIO interrupt handling
├── config/                  # ✅ All configuration files
│   ├── board_config.h       # Pin definitions (Board:: namespace)
│   ├── app_config.h         # Timeouts, thresholds, constants
│   └── version.h            # Firmware version string
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
│   ├── state_machine.h/cpp  # ✅ Main state controller
│   ├── settings.h/cpp       # ✅ User settings management
│   ├── safety.h/cpp         # ✅ Safety monitoring
│   └── pd_manager.h/cpp     # ✅ PD contract management
├── utils/
│   ├── logging.h            # LOG_INFO, LOG_ERROR, etc.
│   ├── eeprom_loader.h/cpp  # ✅ TPS26750 EEPROM flashing
│   └── tps26750_patch.c     # ✅ TPS26750 configuration binary
└── ui/                      # Phase 4 - in progress
    ├── display_manager.h/cpp # ✅ State-based screen rendering
    ├── screens/              # To implement (optional refinement)
    │   ├── screen_boot.h/cpp
    │   ├── screen_main.h/cpp
    │   ├── screen_menu.h/cpp
    │   └── screen_fault.h/cpp
    └── assets/
        └── synapticon_logo.h
```

---

## Code Patterns to Use

### Keep These Patterns ✅
- Hardware Singleton (`extern Hardware hw`)
- Namespace Organization (`Board::`, `AppConfig::`, `Version::`)
- Separate driver layer
- Non-blocking timers (`absolute_time_t`)

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
| Phase 4 | Test all menu navigation paths |
| Phase 5 | Full integration testing |

---

## Notes

- This plan is flexible - adjust as requirements evolve
- Phases can partially overlap
- Testing should happen continuously
- Safety features must be tested with real fault conditions
- Prusa-style input mapping provides intuitive single-control navigation
