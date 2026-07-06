#pragma once
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "board_config.h"
#include "drivers/input/button.h"
#include "drivers/input/rotary_enc.h"
#include "drivers/input/adc_inputs.h"
#include "drivers/rgb_led/sk6812.h"
#include "drivers/gpio/gpio.h"
#include "tps26750.h"
#include "ina228.h"
#include "drivers/display/st7789.h"
#include "drivers/buzzer/buzzer.h"

struct Hardware {
    // Inputs
    // btn1/btn2 are not polled as Button objects (BTN1/BTN2 run through the
    // ISR-flag path in interrupts.cpp). They are retained because their
    // constructors perform the load-bearing GPIO init (gpio_init + input dir +
    // pull config) for the BTN1/BTN2 pads; interrupts::init() only enables the
    // IRQ and does not configure the pads. Fully removing them means migrating
    // that pad init — deferred to the P3.4 button-architecture unification.
    Button btn1;
    Button btn2;
    Button btnEnc;  // Polled via isPressed() for encoder long-press
    SimpleIO overcurrentAlert;  // INA228 ALERT pin (active low)
    SimpleIO pdInterrupt;       // TPS26750 INT pin (active low)
    RotaryEncoder encoder;
    ADCInputs adc;

    // Outputs
    Buzzer buzzer;
    SK6812 rgbLed;       // RGB LED
    SimpleIO en17v;     // 17V EN
    SimpleIO loadSwitch; // Load Switch
    
    // Peripherals
    TPS26750 pdController;
    INA228 powerMonitor;
    ST7789 display;

    Hardware();
    void init();
    void update();

    // True once the INA228 power monitor initialized successfully at boot. When
    // false, current/power sensing and the INA228 hardware overcurrent latch are
    // unavailable, so enabling the load switch would drive an unmonitored output
    // (0V/0A/0C reads look like a plausible idle supply). Callers gate on this
    // before enabling output — see StateMachine::setLoadSwitch.
    bool powerMonitorReady() const { return _power_monitor_ok; }

private:
    bool _power_monitor_ok = false;
};

extern Hardware hw;
