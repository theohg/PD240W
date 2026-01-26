# PD240W Development Plan

## Project Goal

Create an adjustable power supply (0-48V, 0-5A, up to 240W) using USB-C Power Delivery negotiation. The user navigates a menu on an LCD screen using 2 buttons and a rotary encoder to:
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
| **Startup** | Output disabled by default, user enables via button |
| **Overcurrent** | Load switch off, clear INA228 fault latch to recover |
| **Safety** | Overvoltage, overtemperature, power limit enforcement |
| **Startup Sound** | Mario power-up melody |

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

### Phase 3: Application Logic & State Machine (Pending)

**Goal:** Implement control logic and user interaction

#### 3.1 Define Application States
```
BOOT         - Initialization, startup melody
IDLE         - Main screen, output disabled
RUNNING      - Output enabled, monitoring
MENU         - User navigating menu
FAULT        - Protection active (overcurrent/overvoltage/overtemp)
SETTING_*    - User adjusting voltage/current
```

#### 3.2 Implement State Transitions
Transitions based on:
- Button presses
- Encoder rotation
- Safety conditions
- Timeouts (menu → main screen)

#### 3.3 Create Settings Manager
- Target voltage (from available contracts)
- Current limit
- 17V buck enable/disable
- Output enable/disable

#### 3.4 Implement Safety Logic
- Temperature threshold monitoring
- Voltage deviation from target
- Current vs limit monitoring
- Fault state transitions
- Fault recovery (user acknowledgment)

#### 3.5 Add Power Statistics
- Real-time power calculation (V × I)
- Energy accumulation (INA228 energy register)
- Uptime counter
- Min/max tracking

#### 3.6 Integration
- Refactor `main.cpp` to use state machine
- Remove test code

**Files to create:** `src/logic/state_machine.h/cpp`, `settings.h/cpp`, `safety.h/cpp`, `statistics.h/cpp`

---

### Phase 4: User Interface (Pending)

**Goal:** Create intuitive LCD menu and displays

#### 4.1 Design Screen Layouts
- Main screen: V, I, P, Temp display
- Menu screen: List of options
- Setting screens: Voltage selection, current adjustment
- Fault screen: Error message, recovery instructions

#### 4.2 Implement Display Manager
- Screen rendering and transitions
- Layout helpers (draw value with label, draw menu item)

#### 4.3 Implement Screens
- `MainScreen` - Real-time monitoring
- `MenuScreen` - Scrollable menu
- `VoltageSelectScreen` - List of available PD voltages
- `CurrentSetScreen` - Current adjustment with encoder
- `FaultScreen` - Error display with recovery prompt

#### 4.4 Input Handling
- Button 1: Menu/Back
- Button 2: Select/Enable Output
- Encoder: Navigate/Adjust values
- Encoder button: Confirm/Reset fault

#### 4.5 Visual Feedback
- RGB LED: green=OK, yellow=warning, red=fault
- Buzzer beeps for button presses

**Files to create:** `src/ui/display_manager.h/cpp`, `src/ui/screens/*.h/cpp`

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

## File Structure (Target)

```
src/
├── main.cpp
├── hardware.h/cpp
├── interrupts.h/cpp
├── board_config.h
├── drivers/           # ✅ Complete
├── logic/             # Phase 3
│   ├── state_machine.h/cpp
│   ├── settings.h/cpp
│   ├── safety.h/cpp
│   └── statistics.h/cpp
├── ui/                # Phase 4
│   ├── display_manager.h/cpp
│   └── screens/
│       ├── main_screen.h/cpp
│       ├── menu_screen.h/cpp
│       ├── voltage_select_screen.h/cpp
│       ├── current_set_screen.h/cpp
│       └── fault_screen.h/cpp
└── utils/
```

---

## Code Patterns to Use

### Keep These Patterns ✅
- Hardware Singleton (`extern Hardware hw`)
- Namespace Organization (`Board::`, `Config::`)
- Separate driver layer
- Non-blocking timers (`absolute_time_t`)

### Patterns for New Code
- **State Machine:** Explicit state enum with switch/case or state classes
- **Error Handling:** `bool` or `ErrorCode` returns
- **Logging:** Use `LOG_*` macros in application code, not in drivers
- **Configuration Validation:** `static_assert` for compile-time checks

---

## Testing Strategy

| Phase | Testing Approach |
|-------|-----------------|
| Phase 1 | ✅ Each driver tested independently |
| Phase 2 | ✅ TPS26750 tested with USB-C charger, verified with INA228 |
| Phase 3 | Test state transitions manually |
| Phase 4 | Test all menu navigation paths |
| Phase 5 | Full integration testing |

---

## Notes

- This plan is flexible - adjust as requirements evolve
- Phases can partially overlap
- Testing should happen continuously
- Safety features must be tested with real fault conditions
