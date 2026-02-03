#pragma once

#include <cstdint>

// ============================================================================
// Application Configuration Constants
// ============================================================================

namespace AppConfig {
    // -------------------------------------------------------------------------
    // Timing Constants
    // -------------------------------------------------------------------------
    constexpr uint32_t BOOT_DURATION_MS = 2000;        // Boot screen duration [ms]
    constexpr uint32_t MENU_TIMEOUT_MS = 15000;        // Auto-return from menu [ms]
    constexpr uint32_t DISPLAY_UPDATE_MS = 100;        // Main screen refresh rate [ms]
    constexpr uint32_t ENCODER_LONG_PRESS_MS = 700;    // Long press threshold [ms]

    // -------------------------------------------------------------------------
    // Safety Thresholds
    // -------------------------------------------------------------------------
    constexpr uint8_t TEMP_CAUTION_C = 50;             // Temperature hot threshold [C]
    constexpr uint8_t TEMP_WARNING_C = 65;             // Temperature warning threshold [C]
    constexpr uint8_t TEMP_CRITICAL_WARNING_C = 75;    // Temperature critical warning threshold [C]
    constexpr uint8_t TEMP_SHUTDOWN_C = 80;            // Temperature shutdown threshold [C]
    constexpr uint32_t MIN_VBUS_FOR_17V_MV = 18000;    // Minimum VBUS for 17V buck enable [mV]

    // -------------------------------------------------------------------------
    // Current Limit Settings
    // -------------------------------------------------------------------------
    constexpr uint32_t CURRENT_LIMIT_MIN_MA = 10;       // Minimum current limit [mA] (0.01A)
    constexpr uint32_t CURRENT_LIMIT_MAX_MA = 5000;     // Maximum current limit [mA]
    constexpr uint32_t CURRENT_LIMIT_NON_PD_MAX_MA = 3000; // Max for non-PD chargers (USB BC1.2)
    constexpr uint32_t CURRENT_LIMIT_STEP_MA = 10;      // Base adjustment step [mA] (fine control)
    constexpr uint32_t CURRENT_LIMIT_DEFAULT_MA = 1000; // Default current limit [mA]
    constexpr uint32_t CURRENT_LIMIT_VELOCITY_DIV = 1;  // Velocity divider (1 = use full velocity scaling)

    // -------------------------------------------------------------------------
    // PPS Voltage Settings
    // -------------------------------------------------------------------------
    constexpr uint32_t PPS_VOLTAGE_STEP_MV = 20;        // PPS voltage step size [mV] (PD spec minimum)
    constexpr uint32_t PPS_VELOCITY_MULT = 2;           // Velocity multiplier (faster scaling for large range)

    // -------------------------------------------------------------------------
    // Display Settings
    // -------------------------------------------------------------------------
    constexpr uint8_t LCD_BRIGHTNESS_DEFAULT = 100;    // Default brightness (%)
    constexpr uint8_t LCD_BRIGHTNESS_DIM = 5;          // Dimmed brightness (%)
    constexpr uint16_t LCD_WIDTH = 240;
    constexpr uint16_t LCD_HEIGHT = 320;

    // -------------------------------------------------------------------------
    // RGB LED Settings
    // -------------------------------------------------------------------------
    constexpr uint8_t RGB_LED_BRIGHTNESS_NORMAL = 50;  // Normal brightness (%)
    constexpr uint8_t RGB_LED_BRIGHTNESS_DIM = 2;      // Dimmed brightness (%)
}
