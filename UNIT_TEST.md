# PD240W Unit Testing Implementation Plan

Host-based unit testing for the PD240W RP2040 firmware using compile-time hardware abstraction.

---

## 1. Overview & Goals

**Problem:** All PD240W code is tightly coupled to Pico SDK hardware calls. No tests exist — every change requires flashing to hardware.

**Solution:** Refactor drivers into **template classes** parameterized on Hardware Abstraction Layer (HAL) types. On the target (RP2040), templates are instantiated with real Pico SDK wrappers. On the host (x86/ARM macOS/Linux), templates are instantiated with mock HAL types — no Pico SDK needed.

**Goals:**
- Test all logic modules on host without hardware: Settings, Safety, PdManager, StateMachine, EEPROM Workflow
- Test driver register logic (INA228, TPS26750) with mock I2C
- Test input debouncing (Button) and velocity calculation (RotaryEncoder) with mock GPIO/Timer
- Catch undefined behavior and memory issues with sanitizers
- Enforce code quality with static analysis and formatting
- Measure test coverage with gcov/lcov
- Automate via CI/CD

**Constraints (embedded):**
- No dynamic memory in production code (heap-free on target)
- No exceptions, no RTTI
- C++17 standard
- Mocks (host-only) may use std containers freely

---

## 2. Architecture: Template-Based Drivers

### Current Structure (per driver)
```
driver.h    → class definition + declarations
driver.cpp  → method implementations (calls Pico SDK)
```

### New Structure (per driver)
```
driver.h      → template class definition + declarations (template<typename HAL>)
driver_impl.h → template method implementations (included at bottom of driver.h)
```

No `.cpp` files for drivers — template code must live in headers. The compiler instantiates methods only when used.

### Example: INA228 Refactoring

**Before (`ina228.h` + `ina228.cpp`):**
```cpp
// ina228.h
class INA228 {
public:
    INA228(uint8_t address, i2c_inst_t* i2c, float shunt, float maxI);
    float getBusVoltage();
private:
    i2c_inst_t* _i2c;
    bool _readRegister(uint8_t reg, uint8_t* data, uint8_t len);
};

// ina228.cpp
#include "hardware/i2c.h"
bool INA228::_readRegister(uint8_t reg, uint8_t* data, uint8_t len) {
    i2c_write_blocking(_i2c, _address, &reg, 1, true);
    return i2c_read_blocking(_i2c, _address, data, len, false) == len;
}
```

**After (`ina228.h` + `ina228_impl.h`):**
```cpp
// ina228.h
template<typename I2CHal>
class INA228 {
public:
    INA228(uint8_t address, I2CHal& i2c, float shunt, float maxI);
    float getBusVoltage();
private:
    I2CHal& _i2c;
    uint8_t _address;
    bool _readRegister(uint8_t reg, uint8_t* data, uint8_t len);
};

#include "ina228_impl.h"

// ina228_impl.h
template<typename I2CHal>
bool INA228<I2CHal>::_readRegister(uint8_t reg, uint8_t* data, uint8_t len) {
    _i2c.write_blocking(_address, &reg, 1, true);
    return _i2c.read_blocking(_address, data, len, false) == len;
}
```

---

## 3. HAL Abstraction Design

Each HAL is a **concept** (documented via comments/static_assert for C++17 compatibility). Any struct providing the required methods satisfies the HAL.

### 3.1 I2CHal

Used by: `INA228`, `TPS26750`

```cpp
struct I2CHal {
    // Returns number of bytes written, or PICO_ERROR_GENERIC on failure
    int write_blocking(uint8_t addr, const uint8_t* src, size_t len, bool nostop);
    // Returns number of bytes read, or PICO_ERROR_GENERIC on failure
    int read_blocking(uint8_t addr, uint8_t* dst, size_t len, bool nostop);
};
```

**Pico implementation (`hal/pico_i2c.h`):**
```cpp
struct PicoI2C {
    i2c_inst_t* inst;
    PicoI2C(i2c_inst_t* i) : inst(i) {}
    int write_blocking(uint8_t addr, const uint8_t* src, size_t len, bool nostop) {
        return i2c_write_blocking(inst, addr, src, len, nostop);
    }
    int read_blocking(uint8_t addr, uint8_t* dst, size_t len, bool nostop) {
        return i2c_read_blocking(inst, addr, dst, len, nostop);
    }
};
```

### 3.2 SPIHal

Used by: `ST7789`

```cpp
struct SPIHal {
    int write_blocking(const uint8_t* src, size_t len);
};
```

### 3.3 GPIOHal

Used by: `Button`, `RotaryEncoder`, `SimpleIO`, `ST7789`, `Buzzer`

```cpp
struct GPIOHal {
    void init(uint pin);
    void set_dir(uint pin, bool out);
    void put(uint pin, bool value);
    bool get(uint pin);
    void pull_up(uint pin);
    void pull_down(uint pin);
    void disable_pulls(uint pin);
    void set_function(uint pin, uint func);  // GPIO_FUNC_PWM, etc.
    void xor_mask(uint32_t mask);
};
```

### 3.4 PWMHal

Used by: `Buzzer`, `ST7789`

```cpp
struct PWMHal {
    uint gpio_to_slice_num(uint pin);
    uint gpio_to_channel(uint pin);
    void set_wrap(uint slice, uint16_t wrap);
    void set_clkdiv(uint slice, float div);
    void set_enabled(uint slice, bool enabled);
    void set_chan_level(uint slice, uint chan, uint16_t level);
    void set_gpio_level(uint pin, uint16_t level);
    void init(uint slice, /* config */ void* cfg, bool start);
};
```

### 3.5 PIOHal

Used by: `SK6812`

```cpp
struct PIOHal {
    bool can_add_program(const void* program);
    uint add_program(const void* program);
    int claim_unused_sm(bool required);
    void sm_put_blocking(uint sm, uint32_t data);
    void program_init(uint sm, uint offset, uint pin);  // Wraps sk6812_program_init
};
```

### 3.6 ADCHal

Used by: `ADCInputs`

```cpp
struct ADCHal {
    void init();
    void gpio_init(uint pin);
    void select_input(uint input);
    uint16_t read();
};
```

### 3.7 TimerHal

Used by: `Button`, `RotaryEncoder`, `SimpleIO`, `Buzzer`, `SK6812`, + logic modules

```cpp
struct TimerHal {
    uint64_t get_time_us();                           // Replaces get_absolute_time + to_us
    int64_t diff_us(uint64_t t1, uint64_t t2);        // Replaces absolute_time_diff_us
    uint64_t time_us_64();                             // Microsecond counter
    uint32_t to_ms(uint64_t t);                        // Replaces to_ms_since_boot
    uint64_t make_timeout_ms(uint32_t ms);             // Replaces make_timeout_time_ms
    int32_t add_alarm_ms(uint32_t ms, void* cb, void* data, bool fire_if_past);
    void cancel_alarm(int32_t id);
};
```

> **Note:** `absolute_time_t` is a Pico SDK type (struct wrapping `uint64_t`). In the HAL, timers use raw `uint64_t` microsecond values. The Pico HAL implementation converts between the two.

### 3.8 FlashHal

Used by: `Settings`

```cpp
struct FlashHal {
    void erase(uint32_t offset, size_t size);
    void program(uint32_t offset, const uint8_t* data, size_t size);
    uint32_t save_and_disable_interrupts();
    void restore_interrupts(uint32_t status);
    const uint8_t* read_address(uint32_t offset);  // Returns pointer to flash memory
};
```

### 3.9 ClockHal

Used by: `Buzzer` (for `clock_get_hz(clk_sys)`)

```cpp
struct ClockHal {
    uint32_t get_sys_clock_hz();
};
```

---

## 4. Driver Refactoring Plan

### 4.1 INA228 (`src/drivers/power/ina228/`)

| Item | Details |
|------|---------|
| Template params | `I2CHal` |
| Current files | `ina228.h` (declarations, 300+ lines), `ina228.cpp` (implementations) |
| New files | `ina228.h` (template class + `#include "ina228_impl.h"`), `ina228_impl.h` (all method bodies) |
| Remove | `ina228.cpp` |
| Pico SDK to replace | `i2c_write_blocking()` → `_i2c.write_blocking()`, `i2c_read_blocking()` → `_i2c.read_blocking()` |
| Constructor change | `INA228(uint8_t addr, i2c_inst_t* i2c, ...)` → `INA228(uint8_t addr, I2CHal& i2c, ...)` |
| Notes | All public getters/setters call through `_readRegister`/`_writeRegister` — only these two private methods touch the HAL |

### 4.2 TPS26750 (`src/drivers/power/tps26750/`)

| Item | Details |
|------|---------|
| Template params | `I2CHal` |
| Current files | `tps26750.h` (370+ lines), `tps26750.cpp` |
| New files | `tps26750.h` (template class), `tps26750_impl.h` (method bodies) |
| Remove | `tps26750.cpp` |
| Pico SDK to replace | Same as INA228 — only `readRegister()`/`writeRegister()` touch I2C |
| Constructor change | `TPS26750(i2c_inst_t* i2c, uint8_t addr)` → `TPS26750(I2CHal& i2c, uint8_t addr)` |
| Notes | Unique I2C protocol (byte count in payload). PDO parsing is pure logic — very testable |

### 4.3 Button (`src/drivers/input/`)

| Item | Details |
|------|---------|
| Template params | `GPIOHal`, `TimerHal` |
| Current files | `button.h`, `button.cpp` |
| New files | `button.h` (template class), `button_impl.h` |
| Remove | `button.cpp` |
| Pico SDK to replace | `gpio_init/set_dir/pull_up/pull_down/get` → `_gpio.*`, `get_absolute_time/absolute_time_diff_us` → `_timer.*` |
| Constructor change | `Button(uint p, ...)` → `Button(GPIOHal& gpio, TimerHal& timer, uint p, ...)` |

### 4.4 RotaryEncoder (`src/drivers/input/`)

| Item | Details |
|------|---------|
| Template params | `GPIOHal`, `TimerHal` |
| Current files | `rotary_enc.h`, `rotary_enc.cpp` |
| New files | `rotary_enc.h`, `rotary_enc_impl.h` |
| Remove | `rotary_enc.cpp` |
| Pico SDK to replace | `gpio_init/set_dir/disable_pulls/get` → `_gpio.*`, `time_us_64()` → `_timer.time_us_64()` |
| Notes | `handleISR()` uses `gpio_get()` and `time_us_64()` — both go through HAL |

### 4.5 ADCInputs (`src/drivers/input/`)

| Item | Details |
|------|---------|
| Template params | `ADCHal` |
| Current files | `adc_inputs.h`, `adc_inputs.cpp` |
| New files | `adc_inputs.h`, `adc_inputs_impl.h` |
| Remove | `adc_inputs.cpp` |
| Pico SDK to replace | `adc_init/adc_gpio_init/adc_select_input/adc_read` → `_adc.*` |
| Notes | NTC temperature calculation (Steinhart-Hart) is pure math — highly testable |

### 4.6 SimpleIO (`src/drivers/gpio/`)

| Item | Details |
|------|---------|
| Template params | `GPIOHal`, `TimerHal` |
| Current files | `gpio.h`, `gpio.cpp` |
| New files | `gpio.h`, `gpio_impl.h` |
| Remove | `gpio.cpp` |
| Pico SDK to replace | `gpio_init/set_dir/put/get/xor_mask` → `_gpio.*`, `get_absolute_time/to_ms_since_boot` → `_timer.*` |
| Notes | Non-blocking blink logic is pure state machine — testable |

### 4.7 Buzzer (`src/drivers/buzzer/`)

| Item | Details |
|------|---------|
| Template params | `GPIOHal`, `PWMHal`, `TimerHal`, `ClockHal` |
| Current files | `buzzer.h`, `buzzer.cpp` |
| New files | `buzzer.h`, `buzzer_impl.h` |
| Remove | `buzzer.cpp` |
| Pico SDK to replace | `gpio_set_function` → `_gpio.*`, `pwm_*` → `_pwm.*`, `clock_get_hz` → `_clock.*`, `add_alarm_in_ms/cancel_alarm` → `_timer.*` |
| Notes | Melody data arrays and helper functions (`getStartupMelody*`) remain non-template free functions |

### 4.8 SK6812 (`src/drivers/rgb_led/`)

| Item | Details |
|------|---------|
| Template params | `PIOHal`, `TimerHal` |
| Current files | `sk6812.h`, `sk6812.cpp` |
| New files | `sk6812.h`, `sk6812_impl.h` |
| Remove | `sk6812.cpp` |
| Pico SDK to replace | `pio_*` → `_pio.*`, `to_ms_since_boot/get_absolute_time` → `_timer.*` |
| Notes | Color math and blink state machine are pure logic. PIO program init wrapped in HAL |

### 4.9 ST7789 (`src/drivers/display/`)

| Item | Details |
|------|---------|
| Template params | `SPIHal`, `GPIOHal`, `PWMHal` |
| Current files | `st7789.h`, `st7789.cpp` |
| New files | `st7789.h`, `st7789_impl.h` |
| Remove | `st7789.cpp` |
| Pico SDK to replace | `spi_write_blocking` → `_spi.*`, `gpio_put/init/set_dir/set_function` → `_gpio.*`, `pwm_*` → `_pwm.*` |
| Notes | Drawing functions (fillRect, drawLine, drawStringAA, etc.) are pure logic that call `writeCommand`/`writeData` — only those two touch the HAL. `sleep_ms()` in `init()` → `_timer.sleep_ms()` or inline delay |

---

## 5. Platform Traits & Hardware Struct

### Platform Traits

```cpp
// src/hal/platform.h

// Production (RP2040)
struct PicoPlatform {
    using I2C    = PicoI2C;
    using SPI    = PicoSPI;
    using GPIO   = PicoGPIO;
    using PWM    = PicoPWM;
    using PIO    = PicoPIO;
    using ADC    = PicoADC;
    using Timer  = PicoTimer;
    using Flash  = PicoFlash;
    using Clock  = PicoClock;
};

// Test (host)
struct MockPlatform {
    using I2C    = MockI2C;
    using SPI    = MockSPI;
    using GPIO   = MockGPIO;
    using PWM    = MockPWM;
    using PIO    = MockPIO;
    using ADC    = MockADC;
    using Timer  = MockTimer;
    using Flash  = MockFlash;
    using Clock  = MockClock;
};
```

### Hardware Struct (Templated)

```cpp
// src/hardware.h
template<typename P>  // P = Platform traits
struct HardwareT {
    // HAL instances (owned by Hardware, passed to drivers by reference)
    typename P::I2C   i2c0;
    typename P::I2C   i2c1;
    typename P::SPI   spi0;
    typename P::GPIO  gpio;
    typename P::PWM   pwm;
    typename P::PIO   pio;
    typename P::ADC   adc_hal;
    typename P::Timer timer;
    typename P::Clock clock;

    // Drivers (templated on HAL types)
    Button<typename P::GPIO, typename P::Timer>                        btn1;
    Button<typename P::GPIO, typename P::Timer>                        btn2;
    Button<typename P::GPIO, typename P::Timer>                        btnEnc;
    SimpleIO<typename P::GPIO, typename P::Timer>                      overcurrentAlert;
    SimpleIO<typename P::GPIO, typename P::Timer>                      pdInterrupt;
    RotaryEncoder<typename P::GPIO, typename P::Timer>                 encoder;
    ADCInputs<typename P::ADC>                                         adc;
    Buzzer<typename P::GPIO, typename P::PWM, typename P::Timer, typename P::Clock> buzzer;
    SK6812<typename P::PIO, typename P::Timer>                         rgbLed;
    SimpleIO<typename P::GPIO, typename P::Timer>                      debugLed;
    SimpleIO<typename P::GPIO, typename P::Timer>                      EN_17V;
    SimpleIO<typename P::GPIO, typename P::Timer>                      loadSwitch;
    TPS26750<typename P::I2C>                                          pdController;
    INA228<typename P::I2C>                                            powerMonitor;
    ST7789<typename P::SPI, typename P::GPIO, typename P::PWM>         display;

    HardwareT();   // Initializes all drivers with HAL references + pin numbers
    void init();
    void update();
};

// Platform selection
#ifdef UNIT_TEST_BUILD
using Hardware = HardwareT<MockPlatform>;
#else
using Hardware = HardwareT<PicoPlatform>;
#endif

extern Hardware hw;
```

### Impact on Logic Layer

Logic modules (`safety.cpp`, `pd_manager.cpp`, `state_machine.cpp`, `display_manager.cpp`) access hardware via `hw.powerMonitor.getBusVoltage()`, `hw.adc.getVBUS()`, etc. — these calls remain identical regardless of platform. **No changes needed in logic code** beyond the `Hardware` typedef.

The only exception is `settings.cpp` which calls `flash_range_erase`/`flash_range_program` directly. This needs a `FlashHal` member or a `Settings` template parameter.

---

## 6. Mock Implementations

Mocks run on host only. They may use `std::vector`, `std::map`, etc.

### 6.1 MockI2C

```cpp
struct MockI2C {
    // Register simulation: address → register → data
    std::map<uint8_t, std::map<uint8_t, std::vector<uint8_t>>> registers;
    bool fail_next = false;  // Simulate I2C failure

    int write_blocking(uint8_t addr, const uint8_t* src, size_t len, bool nostop) {
        if (fail_next) { fail_next = false; return PICO_ERROR_GENERIC; }
        // First byte is register address (for reads), store remaining as write data
        if (len > 1) {
            uint8_t reg = src[0];
            registers[addr][reg].assign(src + 1, src + len);
        }
        last_write_addr = addr;
        last_write_reg = src[0];
        return (int)len;
    }

    int read_blocking(uint8_t addr, uint8_t* dst, size_t len, bool nostop) {
        if (fail_next) { fail_next = false; return PICO_ERROR_GENERIC; }
        auto& reg_data = registers[addr][last_write_reg];
        size_t copy_len = std::min(len, reg_data.size());
        std::memcpy(dst, reg_data.data(), copy_len);
        return (int)len;
    }

    // Test helpers
    void setRegister(uint8_t addr, uint8_t reg, std::initializer_list<uint8_t> data);
    uint8_t last_write_addr = 0, last_write_reg = 0;
};
```

### 6.2 MockGPIO

```cpp
struct MockGPIO {
    std::map<uint, bool> pin_state;      // Current output/input state
    std::map<uint, bool> pin_direction;  // true = output
    std::map<uint, uint> pin_function;   // GPIO_FUNC_PWM, etc.

    void init(uint pin) { pin_state[pin] = false; }
    void set_dir(uint pin, bool out) { pin_direction[pin] = out; }
    void put(uint pin, bool value) { pin_state[pin] = value; }
    bool get(uint pin) { return pin_state[pin]; }
    void pull_up(uint pin) {}
    void pull_down(uint pin) {}
    void disable_pulls(uint pin) {}
    void set_function(uint pin, uint func) { pin_function[pin] = func; }
    void xor_mask(uint32_t mask) {
        for (uint i = 0; i < 30; i++) {
            if (mask & (1u << i)) pin_state[i] = !pin_state[i];
        }
    }

    // Test helper: simulate external input
    void simulateInput(uint pin, bool value) { pin_state[pin] = value; }
};
```

### 6.3 MockTimer

```cpp
struct MockTimer {
    uint64_t current_time_us = 0;

    uint64_t get_time_us() { return current_time_us; }
    int64_t diff_us(uint64_t t1, uint64_t t2) { return (int64_t)(t2 - t1); }
    uint64_t time_us_64() { return current_time_us; }
    uint32_t to_ms(uint64_t t) { return (uint32_t)(t / 1000); }
    uint64_t make_timeout_ms(uint32_t ms) { return current_time_us + ms * 1000ULL; }
    int32_t add_alarm_ms(uint32_t ms, void* cb, void* data, bool fire) { return 1; }
    void cancel_alarm(int32_t id) {}

    // Test helper: advance time
    void advanceMs(uint32_t ms) { current_time_us += ms * 1000ULL; }
    void advanceUs(uint64_t us) { current_time_us += us; }
};
```

### 6.4 MockFlash

```cpp
struct MockFlash {
    std::array<uint8_t, 4096> sector{};  // Simulates last flash sector
    bool erase_called = false;
    bool program_called = false;

    void erase(uint32_t offset, size_t size) {
        std::fill(sector.begin(), sector.end(), 0xFF);
        erase_called = true;
    }
    void program(uint32_t offset, const uint8_t* data, size_t size) {
        std::memcpy(sector.data(), data, std::min(size, sector.size()));
        program_called = true;
    }
    uint32_t save_and_disable_interrupts() { return 0; }
    void restore_interrupts(uint32_t) {}
    const uint8_t* read_address(uint32_t offset) { return sector.data(); }
};
```

### 6.5 Other Mocks

`MockSPI`, `MockPWM`, `MockPIO`, `MockADC`, `MockClock` follow the same pattern — store state, provide test helpers, no real hardware interaction. `MockADC` is notable:

```cpp
struct MockADC {
    uint16_t channel_values[4] = {0};  // Simulated ADC readings per channel
    uint8_t selected = 0;

    void init() {}
    void gpio_init(uint pin) {}
    void select_input(uint input) { selected = input; }
    uint16_t read() { return channel_values[selected]; }

    // Test helper: set voltage in mV, auto-converts to 12-bit ADC value
    void setVoltage(uint channel, float voltage_v) {
        channel_values[channel] = (uint16_t)(voltage_v / 3.3f * 4095.0f);
    }
};
```

---

## 7. Test Framework: Snitch

[Snitch](https://github.com/snitch-org/snitch) is a lightweight C++20 test framework with no heap allocation overhead. Compatible with C++17 for core features.

### Integration via CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(snitch
    GIT_REPOSITORY https://github.com/snitch-org/snitch.git
    GIT_TAG        v1.2.5
)
FetchContent_MakeAvailable(snitch)
```

### Test Syntax

```cpp
#include <snitch/snitch.hpp>

TEST_CASE("Settings: current limit clamping", "[settings]") {
    // ... setup ...
    REQUIRE(settings.getCurrentLimit() == 1000);

    SECTION("below minimum clamps to MIN_MA") {
        settings.setCurrentLimit(5);
        REQUIRE(settings.getCurrentLimit() == AppConfig::CURRENT_LIMIT_MIN_MA);
    }

    SECTION("above maximum clamps to MAX_MA") {
        settings.setCurrentLimit(6000);
        REQUIRE(settings.getCurrentLimit() == AppConfig::CURRENT_LIMIT_MAX_MA);
    }
}
```

---

## 8. Test Cases by Module

### 8.1 Settings (`src/logic/settings.h/cpp`)

| Test | What it verifies |
|------|-----------------|
| `setCurrentLimit` below min → clamps to `CURRENT_LIMIT_MIN_MA` (10) | Input validation |
| `setCurrentLimit` above max → clamps to `CURRENT_LIMIT_MAX_MA` (5000) | Input validation |
| `setCurrentLimit` valid value → stored exactly | Happy path |
| `setCurrentLimit` same value → `_dirty` stays false | No-op detection |
| `setCurrentLimit` different value → `_dirty` becomes true | Change tracking |
| `setLcdBrightness(150)` → clamps to 100 | Input validation |
| `setLcdBrightness(0)` → stores 0 (display code enforces min 5) | Boundary |
| `setAutoDimMinutes(0)` → clamps to 1 | Input validation |
| `setAutoDimMinutes(15)` → clamps to 10 | Input validation |
| `setStartupMelody(5)` → clamps to 3 | Input validation |
| `resetToDefaults()` → all fields match expected defaults | Default values |
| `resetToDefaults()` → `_dirty` is false | State reset |
| CRC32 round-trip: calculate → store → recalculate → matches | Data integrity |
| `saveToFlash` → `FlashHal.erase` + `FlashHal.program` called | Flash interaction |
| `loadFromFlash` with valid magic+version+CRC → returns true | Load success |
| `loadFromFlash` with wrong magic → returns false | Corruption detection |
| `loadFromFlash` with wrong version → returns false | Version migration |
| `loadFromFlash` with wrong CRC → returns false | Integrity check |
| `requestSave` + `update` before debounce → no flash write | Debounce timing |
| `requestSave` + advance 2s + `update` → flash write occurs | Debounce trigger |

### 8.2 Safety (`src/logic/safety.h/cpp`)

| Test | What it verifies |
|------|-----------------|
| Temperature 25°C → `SafetyStatus::OK` | Normal operation |
| Temperature 50°C → `SafetyStatus::CAUTION` | Caution threshold |
| Temperature 49°C after caution → stays `CAUTION` (hysteresis) | Hysteresis active |
| Temperature 48°C after caution → clears to `OK` (2°C hysteresis) | Hysteresis clear |
| Temperature 65°C → `SafetyStatus::WARNING` | Warning threshold |
| Temperature 63°C after warning → stays `WARNING` | Hysteresis active |
| Temperature 80°C → `SafetyStatus::FAULT`, load switch disabled | Shutdown threshold |
| Temperature 78°C after fault → stays `FAULT` | Hysteresis active |
| Temperature 77°C after fault → clears fault | Hysteresis clear |
| VBUS 5V → `pd_connected = true` | Connection detection |
| VBUS 3V → `pd_connected = false` | Disconnect at < 4V |
| VBUS drops from 20V to 3V → load switch disabled | Disconnect response |
| `max_temperature_c` = max(NTC, INA228) | Dual sensor logic |
| Overcurrent alert pin low + switch on → `overcurrent_latched = true` | Overcurrent detection |
| Overcurrent alert pin low + switch off → no latch (false trigger) | False trigger guard |

### 8.3 PdManager (`src/logic/pd_manager.h/cpp`)

| Test | What it verifies |
|------|-----------------|
| `detectPdRevision` with AVS PDO → "PD3.1" | PD revision detection |
| `detectPdRevision` with PPS PDO → "PD3.0" | PD revision detection |
| `detectPdRevision` with fixed-only → "PD2.0" | PD revision detection |
| `detectPdRevision` with 0 PDOs → empty string | Edge case |
| `requestFixedVoltage` success → state = `REQUESTING`, PPS deactivated | State transition |
| `requestPpsVoltage` success → PPS state active, keep-alive timer set | PPS activation |
| `requestFixedVoltage` after PPS → PPS state cleared, correction reset | PPS deactivation |
| Negotiation timeout (2s elapsed) → state = `TIMEOUT` | Timeout handling |
| PPS keep-alive: 7s elapsed → re-request sent | Keep-alive timing |
| PPS auto-tuning: error > 30mV → correction accumulated | Tuning active |
| PPS auto-tuning: error < 30mV → `_pps_tuning_converged = true` | Convergence |
| PPS auto-tuning: correction clamped at ±500mV | Safety clamp |
| PPS voltage rounding to 20mV steps | PD spec compliance |
| `getSourceCapabilities` copies from cache correctly | PDO cache |

### 8.4 EEPROM Workflow (`src/logic/tps_eeprom_workflow.h/cpp`)

| Test | What it verifies |
|------|-----------------|
| `start()` → stage = `COMPARING`, active = true | Initialization |
| Rotate in `CONFIRM` stage → toggles Yes/No | User input |
| Click "No" in `CONFIRM` → returns true (exit) | Cancel flow |
| Click "Yes" in `CONFIRM` → stage = `FLASHING` | Confirm flow |
| `setProgress(0, 50)` → phase=0, progress=50 | Progress tracking |
| `cleanup()` → active = false | Cleanup |

### 8.5 CRC32 (in `settings.cpp`)

| Test | What it verifies |
|------|-----------------|
| Known input → known CRC32 output (standard test vector) | Algorithm correctness |
| All-zeros input → expected CRC | Edge case |
| Single-byte input → expected CRC | Minimum input |
| CRC of `UserSettings` struct with known values → deterministic | Struct layout stability |

### 8.6 Button Debounce (`src/drivers/input/button.h`)

| Test | What it verifies |
|------|-----------------|
| Press + release within debounce window → no click | Bounce rejection |
| Press + hold beyond debounce → `isPressed()` = true | Stable press |
| Press + release + re-read → `isClicked()` returns true once | Edge detection |
| Multiple instances don't share state | Per-instance isolation |

### 8.7 Encoder Velocity (`src/drivers/input/rotary_enc.h`)

| Test | What it verifies |
|------|-----------------|
| Slow rotation (>100ms between ticks) → multiplier = 1 | Base speed |
| Fast rotation (<20ms between ticks) → multiplier > 1 | Acceleration |
| `getVelocityMultiplier()` with various `_tick_interval_us` values | Curve shape |

### 8.8 ADC Temperature Calculation (`src/drivers/input/adc_inputs.h`)

| Test | What it verifies |
|------|-----------------|
| ADC value at 25°C reference → ~25°C output | Calibration point |
| ADC value at high temp → reasonable output (sanity check) | NTC curve |
| VBUS voltage divider scaling: known ADC → correct mV | Divider ratio |

---

## 9. Build System

### Directory Structure

```
test/
├── CMakeLists.txt          # Host-side test build (no Pico SDK)
├── mocks/
│   ├── mock_i2c.h
│   ├── mock_spi.h
│   ├── mock_gpio.h
│   ├── mock_pwm.h
│   ├── mock_pio.h
│   ├── mock_adc.h
│   ├── mock_timer.h
│   ├── mock_flash.h
│   ├── mock_clock.h
│   └── mock_platform.h     # MockPlatform traits struct
├── test_settings.cpp
├── test_safety.cpp
├── test_pd_manager.cpp
├── test_eeprom_workflow.cpp
├── test_crc32.cpp
├── test_button.cpp
├── test_encoder.cpp
└── test_adc_calc.cpp
```

### `test/CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.20)
project(PD240W_Tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# Options
# ============================================================================
option(ENABLE_SANITIZERS "Enable ASan + UBSan" ON)
option(ENABLE_COVERAGE  "Enable gcov coverage" OFF)

# ============================================================================
# Sanitizers
# ============================================================================
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

# ============================================================================
# Coverage
# ============================================================================
if(ENABLE_COVERAGE)
    add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# ============================================================================
# Snitch Test Framework
# ============================================================================
include(FetchContent)
FetchContent_Declare(snitch
    GIT_REPOSITORY https://github.com/snitch-org/snitch.git
    GIT_TAG        v1.2.5
)
FetchContent_MakeAvailable(snitch)

# ============================================================================
# Test Executable
# ============================================================================
add_executable(pd240w_tests
    test_settings.cpp
    test_safety.cpp
    test_pd_manager.cpp
    test_eeprom_workflow.cpp
    test_crc32.cpp
    test_button.cpp
    test_encoder.cpp
    test_adc_calc.cpp
)

target_include_directories(pd240w_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/logic
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ui
)

target_compile_definitions(pd240w_tests PRIVATE UNIT_TEST_BUILD)
target_link_libraries(pd240w_tests PRIVATE snitch::snitch)

# ============================================================================
# CTest Integration
# ============================================================================
enable_testing()
add_test(NAME unit_tests COMMAND pd240w_tests)

# ============================================================================
# Coverage Report Target
# ============================================================================
if(ENABLE_COVERAGE)
    find_program(LCOV lcov)
    find_program(GENHTML genhtml)
    if(LCOV AND GENHTML)
        add_custom_target(coverage
            COMMAND ${CMAKE_CURRENT_BINARY_DIR}/pd240w_tests
            COMMAND ${LCOV} --directory . --capture --output-file coverage.info
                    --ignore-errors mismatch
            COMMAND ${LCOV} --remove coverage.info '*/test/*' '*/snitch/*' '/usr/*'
                    --output-file coverage_filtered.info
            COMMAND ${GENHTML} coverage_filtered.info
                    --output-directory ${CMAKE_CURRENT_BINARY_DIR}/coverage_report
            DEPENDS pd240w_tests
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating coverage report → build/coverage_report/index.html"
        )
    endif()
endif()
```

### Build Commands

```bash
# Build and run tests
cd test && mkdir -p build && cd build
cmake -G Ninja .. && ninja
./pd240w_tests

# With coverage
cmake -G Ninja -DENABLE_COVERAGE=ON .. && ninja
ninja coverage
open coverage_report/index.html

# Without sanitizers (faster)
cmake -G Ninja -DENABLE_SANITIZERS=OFF .. && ninja
```

---

## 10. Sanitizers (ASan + UBSan)

Enabled by default in the test build. Catches:

| Sanitizer | Catches |
|-----------|---------|
| **ASan** (AddressSanitizer) | Buffer overflow, use-after-free, stack overflow, memory leaks |
| **UBSan** (UndefinedBehaviorSanitizer) | Signed integer overflow, null pointer dereference, misaligned access, shift out of bounds, unreachable code |

Both are compile-time flags (`-fsanitize=address,undefined`). They add runtime instrumentation — tests run ~2x slower but catch real bugs.

**Important:** ASan/UBSan only work on host builds. Never enable for the ARM cross-compilation (the RP2040 doesn't support them).

---

## 11. Static Analysis

### clang-tidy

Add a `.clang-tidy` file at project root:

```yaml
Checks: >
  -*,
  bugprone-*,
  modernize-*,
  readability-*,
  performance-*,
  -modernize-use-trailing-return-type,
  -readability-magic-numbers,
  -readability-identifier-length

WarningsAsErrors: 'bugprone-*'
HeaderFilterRegex: 'src/.*'
```

Run manually:
```bash
# Requires compile_commands.json from the test build
cd test/build
run-clang-tidy -p . -header-filter='src/.*' ../../src/
```

### cppcheck

```bash
cppcheck --enable=all --suppress=missingIncludeSystem \
    --project=test/build/compile_commands.json \
    --error-exitcode=1
```

### Integration in CI

Both tools are run in the CI workflow (see Section 14). They don't modify code — they report issues.

---

## 12. Code Formatting (clang-format)

Add `.clang-format` at project root. Suggested starting config:

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
ColumnLimit: 120
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
BreakBeforeBraces: Attach
PointerAlignment: Left
SortIncludes: false
```

### Commands

```bash
# Check formatting (CI — fails if changes needed)
find src test -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

# Fix formatting (dev — modifies files)
find src test -name '*.h' -o -name '*.cpp' | xargs clang-format -i
```

### CMake Target (optional)

Add to `test/CMakeLists.txt`:
```cmake
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE ALL_SOURCES ../src/*.h ../src/*.cpp test_*.cpp mocks/*.h)
    add_custom_target(format     COMMAND ${CLANG_FORMAT} -i ${ALL_SOURCES})
    add_custom_target(check-format COMMAND ${CLANG_FORMAT} --dry-run --Werror ${ALL_SOURCES})
endif()
```

---

## 13. Code Coverage (gcov/lcov)

Enabled via `-DENABLE_COVERAGE=ON`. Workflow:

1. Compile with `--coverage` flags (instruments every branch)
2. Run tests → generates `.gcda` files
3. `lcov` collects data into `coverage.info`
4. `genhtml` renders HTML report

**Coverage target:** The `ninja coverage` target (Section 9) does all steps. Output in `test/build/coverage_report/index.html`.

**What to measure:** Focus on logic layer coverage:
- `src/logic/settings.cpp` — target 90%+
- `src/logic/safety.cpp` — target 80%+ (temperature thresholds, hysteresis paths)
- `src/logic/pd_manager.cpp` — target 70%+ (PD revision, PPS tuning, negotiation states)
- `src/logic/tps_eeprom_workflow.cpp` — target 80%+

Driver template code coverage depends on how many mock-based tests exercise the register logic.

---

## 14. CI/CD Integration (GitHub Actions)

### `.github/workflows/test.yml`

```yaml
name: Build & Test

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  host-tests:
    name: Host Unit Tests
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake ninja-build clang clang-tidy cppcheck lcov

      - name: Build tests
        run: |
          cd test
          mkdir -p build && cd build
          cmake -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
                -DENABLE_SANITIZERS=ON \
                -DENABLE_COVERAGE=ON ..
          ninja

      - name: Run tests
        run: cd test/build && ./pd240w_tests

      - name: Generate coverage report
        run: |
          cd test/build
          lcov --directory . --capture --output-file coverage.info --ignore-errors mismatch
          lcov --remove coverage.info '*/test/*' '*/snitch/*' '/usr/*' --output-file coverage_filtered.info
          genhtml coverage_filtered.info --output-directory coverage_report

      - name: Upload coverage artifact
        uses: actions/upload-artifact@v4
        with:
          name: coverage-report
          path: test/build/coverage_report/

  static-analysis:
    name: Static Analysis
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y cmake ninja-build clang clang-tidy cppcheck

      - name: Build compile_commands.json
        run: |
          cd test
          mkdir -p build && cd build
          cmake -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DENABLE_SANITIZERS=OFF ..
          ninja

      - name: Run clang-tidy
        run: |
          cd test/build
          run-clang-tidy -p . -header-filter='src/.*' ../../src/ || true

      - name: Run cppcheck
        run: |
          cppcheck --enable=all --suppress=missingIncludeSystem \
            --project=test/build/compile_commands.json \
            --error-exitcode=1

  format-check:
    name: Format Check
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install clang-format
        run: sudo apt-get install -y clang-format

      - name: Check formatting
        run: |
          find src test -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

  firmware-build:
    name: Firmware Build (RP2040)
    runs-on: ubuntu-latest
    container:
      image: ghcr.io/raspberrypi/pico-sdk-tools:latest
    steps:
      - uses: actions/checkout@v4

      - name: Build firmware
        run: |
          mkdir -p build && cd build
          cmake -G Ninja ..
          ninja

      - name: Upload .uf2
        uses: actions/upload-artifact@v4
        with:
          name: firmware
          path: build/PD240W.uf2
```

### README Badge

```markdown
[![Build & Test](https://github.com/<owner>/PD240W/actions/workflows/test.yml/badge.svg)](https://github.com/<owner>/PD240W/actions/workflows/test.yml)
```

---

## 15. Implementation Phases

### Phase 0: Setup (no code changes)
- [ ] Create `test/` directory structure
- [ ] Create `test/CMakeLists.txt` (Section 9)
- [ ] Create `.clang-format` (Section 12)
- [ ] Create `.clang-tidy` (Section 11)
- [ ] Add `.github/workflows/test.yml` (Section 14)
- [ ] Verify host build compiles (empty test file + Snitch)

### Phase 1: HAL Layer
- [ ] Create `src/hal/` directory
- [ ] Implement `I2CHal` concept + `PicoI2C` wrapper (`hal/pico_i2c.h`)
- [ ] Implement `SPIHal` + `PicoSPI` (`hal/pico_spi.h`)
- [ ] Implement `GPIOHal` + `PicoGPIO` (`hal/pico_gpio.h`)
- [ ] Implement `PWMHal` + `PicoPWM` (`hal/pico_pwm.h`)
- [ ] Implement `PIOHal` + `PicoPIO` (`hal/pico_pio.h`)
- [ ] Implement `ADCHal` + `PicoADC` (`hal/pico_adc.h`)
- [ ] Implement `TimerHal` + `PicoTimer` (`hal/pico_timer.h`)
- [ ] Implement `FlashHal` + `PicoFlash` (`hal/pico_flash.h`)
- [ ] Implement `ClockHal` + `PicoClock` (`hal/pico_clock.h`)
- [ ] Create `PicoPlatform` traits struct (`hal/platform.h`)

### Phase 2: Refactor Drivers (one at a time, verify firmware still compiles after each)
- [ ] INA228: `.h` + `.cpp` → `.h` + `_impl.h` (template on `I2CHal`)
- [ ] TPS26750: same pattern
- [ ] Button: template on `GPIOHal` + `TimerHal`
- [ ] RotaryEncoder: template on `GPIOHal` + `TimerHal`
- [ ] ADCInputs: template on `ADCHal`
- [ ] SimpleIO: template on `GPIOHal` + `TimerHal`
- [ ] Buzzer: template on `GPIOHal` + `PWMHal` + `TimerHal` + `ClockHal`
- [ ] SK6812: template on `PIOHal` + `TimerHal`
- [ ] ST7789: template on `SPIHal` + `GPIOHal` + `PWMHal`
- [ ] Refactor `Hardware` struct to `HardwareT<Platform>` with `using Hardware = ...`
- [ ] Update `CMakeLists.txt`: remove `.cpp` driver files, keep logic `.cpp` files
- [ ] **Verify firmware builds and works on hardware**

### Phase 3: Mock Layer
- [ ] Create `test/mocks/mock_i2c.h` through `mock_clock.h`
- [ ] Create `test/mocks/mock_platform.h` with `MockPlatform` traits
- [ ] Verify test build compiles with mock Hardware instantiation

### Phase 4: Write Tests (highest value first)
- [ ] `test_settings.cpp` — Settings validation, CRC32, flash persistence
- [ ] `test_safety.cpp` — Temperature thresholds, hysteresis, overcurrent
- [ ] `test_pd_manager.cpp` — PD revision, PPS tuning, negotiation states
- [ ] `test_eeprom_workflow.cpp` — Workflow state machine
- [ ] `test_button.cpp` — Debounce logic
- [ ] `test_encoder.cpp` — Velocity multiplier
- [ ] `test_adc_calc.cpp` — NTC temperature, voltage divider math

### Phase 5: CI/CD & Polish
- [ ] Enable GitHub Actions workflow
- [ ] Add coverage badge to README
- [ ] Set up devcontainer (see DEV_CONTAINER.md)
- [ ] Run clang-format on entire codebase (one-time, single commit)
- [ ] Tune clang-tidy checks based on initial results
