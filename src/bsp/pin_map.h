#pragma once

namespace Board {
    // Buttons & Inputs
    constexpr int PIN_BTN_1          = 0;
    constexpr int PIN_BTN_2          = 2;
    constexpr int PIN_ENC_A          = 7;
    constexpr int PIN_ENC_BTN        = 8;
    constexpr int PIN_SWITCH_EN_READ = 12;
    constexpr int PIN_ENC_B          = 13;

    // I2C (I2C0)
    constexpr int PIN_I2C_SDA   = 4;
    constexpr int PIN_I2C_SCL   = 5;

    // SPI (SPI1 for Display)
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
}
