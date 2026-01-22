#pragma once

namespace Config {
    // System Settings
    constexpr float ADC_REF_VOLTAGE = 3.3f;
    constexpr int ADC_RESOLUTION    = 4096;
    constexpr float ADC_CONVERT     = ADC_REF_VOLTAGE / ADC_RESOLUTION;

    // UI / LED Settings
    constexpr bool LED_IS_RGBW      = false;
    constexpr int LED_FREQ          = 800000;

    // NTC Settings
    constexpr float NTC_BETA            = 3950.0f;  // Beta value
    constexpr float NTC_REF_TEMP_C      = 25.0f;    // Reference temperature in Celsius
    constexpr float NTC_REF_RESISTOR    = 10000.0f; // Resistance at reference temperature
    constexpr float NTC_SERIES_RESISTOR = 4700.0f;  // Series resistor value

    // Voltage Divider Settings
    constexpr float VOLTAGE_DIVIDER_TOP = 150000.0f; // Top resistor value
    constexpr float VOLTAGE_DIVIDER_BOT = 10000.0f;  // Bottom resistor value
}