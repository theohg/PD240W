#pragma once

#include <cstdint>

// ============================================================================
// Board Configuration - PD240W
// Combines pin definitions and system constants
// ============================================================================

namespace Board {
    // -------------------------------------------------------------------------
    // GPIO Pin Definitions
    // -------------------------------------------------------------------------

    // Buttons & Inputs
    constexpr int PIN_BTN_1          = 0;
    constexpr int PIN_BTN_2          = 2;
    constexpr int PIN_ENC_A          = 7;
    constexpr int PIN_ENC_BTN        = 8;
    constexpr int PIN_USB_PD_IRQ     = 9;
    constexpr int PIN_SWITCH_EN_READ = 12;
    constexpr int PIN_ENC_B          = 13;

    // I2C (I2C0)
    constexpr int PIN_I2C_SDA   = 4;
    constexpr int PIN_I2C_SCL   = 5;

    // SPI (SPI0 for Display)
    constexpr int PIN_LCD_CS    = 17;
    constexpr int PIN_LCD_SCK   = 18;
    constexpr int PIN_LCD_MOSI  = 19;
    constexpr int PIN_LCD_DC    = 21;
    constexpr int PIN_LCD_RST   = 22;
    constexpr int PIN_LCD_BL    = 24;

    // Outputs
    constexpr int PIN_DEBUG_LED = 1;
    constexpr int PIN_SWITCH_EN = 3;
    constexpr int PIN_BUZZER    = 6;
    constexpr int PIN_17V_EN    = 20;
    constexpr int PIN_RGB_LED   = 28;

    // Analog
    constexpr int PIN_ADC_VOLT  = 26;
    constexpr int PIN_ADC_TEMP  = 27;
    constexpr int ADC_CH_VOLTAGE = 0;
    constexpr int ADC_CH_TEMP    = 1;

    // Extra GPIOs routed to solder pads for user access
    constexpr int PIN_GPIO_14    = 14;
    constexpr int PIN_GPIO_15    = 15;
    constexpr int PIN_GPIO_16    = 16;  // Also used for UART TX
    constexpr int PIN_GPIO_29    = 29;  // Also used for UART RX

    // -------------------------------------------------------------------------
    // I2C Parameters
    // -------------------------------------------------------------------------
    constexpr uint8_t  I2C_ADDR_INA228   = 0x40;   // 7-bit address for INA228
    constexpr uint8_t  I2C_ADDR_TPS26750 = 0x21;   // 7-bit address for TPS26750
    constexpr uint32_t I2C_SPEED_HZ      = 400000; // 400kHz Fast Mode

    // -------------------------------------------------------------------------
    // SPI Parameters
    // -------------------------------------------------------------------------
    constexpr uint32_t SPI_SPEED_HZ = 10000000; // 10MHz

    // -------------------------------------------------------------------------
    // ADC Settings
    // -------------------------------------------------------------------------
    constexpr float ADC_REF_VOLTAGE = 3.3f;
    constexpr int   ADC_RESOLUTION  = 4096;
    constexpr float ADC_CONVERT     = ADC_REF_VOLTAGE / ADC_RESOLUTION;

    // -------------------------------------------------------------------------
    // RGB LED Settings (SK6812)
    // -------------------------------------------------------------------------
    constexpr bool LED_IS_RGBW = false;
    constexpr int  LED_FREQ    = 800000;

    // -------------------------------------------------------------------------
    // NTC Thermistor Settings
    // -------------------------------------------------------------------------
    constexpr float NTC_BETA            = 3950.0f;  // Beta value
    constexpr float NTC_REF_TEMP_C      = 25.0f;    // Reference temperature in Celsius
    constexpr float NTC_REF_RESISTOR    = 10000.0f; // Resistance at reference temperature
    constexpr float NTC_SERIES_RESISTOR = 4700.0f;  // Series resistor value

    // -------------------------------------------------------------------------
    // Voltage Divider Settings (VBUS measurement)
    // -------------------------------------------------------------------------
    constexpr float VOLTAGE_DIVIDER_TOP = 150000.0f; // Top resistor value (150kΩ)
    constexpr float VOLTAGE_DIVIDER_BOT = 10000.0f;  // Bottom resistor value (10kΩ)

    // -------------------------------------------------------------------------
    // INA228 Power Monitor Settings
    // -------------------------------------------------------------------------
    constexpr float    INA228_SHUNT_RESISTOR          = 0.008f; // 8mΩ shunt resistor
    constexpr bool     INA228_USE_LOW_ADC_RANGE       = true;   // 41mV shunt range (~5.12A with 8mΩ)
    constexpr float    INA228_MEASUREMENT_MAX_CURRENT = 5.12f;  // INA full-scale used for calibration
    constexpr uint16_t INA228_SHUNT_TEMPCO_PPM        = 50;     // Shunt resistor temperature coefficient [ppm/°C]
}
