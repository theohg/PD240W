#pragma once

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "board_config.h"
#include <math.h>

// ADC Input Driver for PD240W
// Handles voltage measurement (pre-switch VBUS) and temperature (NTC thermistor)

class ADCInputs {
private:
    // Averaging for noise reduction
    static constexpr uint8_t AVERAGING_SAMPLES = 16;

    // Helper: Read raw ADC with averaging
    uint16_t readADCRaw(uint8_t channel) const;

public:
    // Initialization
    bool init();

    // Raw ADC reading (12-bit: 0-4095)
    uint16_t readADC(uint8_t channel) const;

    // Generic voltage reading with ADC reference scaling
    float readVoltage(uint8_t channel) const;

    // VBUS voltage measurement (pre-switch, with voltage divider compensation)
    // Voltage divider: 150k / 10k, so V_bus = V_adc * 16
    float getVBUS() const;

    // NTC thermistor temperature measurement
    // Returns temperature in Celsius
    // Uses simplified Steinhart-Hart (Beta parameter equation)
    float getTemperature() const;
};