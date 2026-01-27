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
    constexpr uint32_t MENU_TIMEOUT_MS = 10000;        // Auto-return from menu [ms]
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
    constexpr uint32_t CURRENT_LIMIT_MIN_MA = 100;      // Minimum current limit [mA]
    constexpr uint32_t CURRENT_LIMIT_MAX_MA = 5000;     // Maximum current limit [mA]
    constexpr uint32_t CURRENT_LIMIT_STEP_MA = 100;     // Adjustment step size  [mA]
    constexpr uint32_t CURRENT_LIMIT_DEFAULT_MA = 1000; // Default current limit [mA]

    // -------------------------------------------------------------------------
    // Display Settings
    // -------------------------------------------------------------------------
    constexpr uint8_t LCD_BRIGHTNESS_DEFAULT = 100;    // Default brightness (%)
    constexpr uint16_t LCD_WIDTH = 240;
    constexpr uint16_t LCD_HEIGHT = 320;
}
