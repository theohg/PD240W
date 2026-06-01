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
#include "drivers/power/tps26750/tps26750.h"
#include "ina228.h"
#include "drivers/display/st7789.h"
#include "drivers/buzzer/buzzer.h"

struct Hardware {
    // Inputs
    Button btn1;
    Button btn2;
    Button btnEnc;
    SimpleIO overcurrentAlert;  // INA228 ALERT pin (active low)
    SimpleIO pdInterrupt;       // TPS26750 INT pin (active low)
    RotaryEncoder encoder;
    ADCInputs adc;

    // Outputs
    Buzzer buzzer;
    SK6812 rgbLed;       // RGB LED
    SimpleIO EN_17V;     // 17V EN
    SimpleIO loadSwitch; // Load Switch
    
    // Peripherals
    TPS26750 pdController;
    INA228 powerMonitor;
    ST7789 display;

    Hardware();
    void init();
    void update(); 
};

extern Hardware hw;
