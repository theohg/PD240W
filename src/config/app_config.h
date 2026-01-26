#pragma once

#include <cstdint>

// ============================================================================
// Application Configuration Constants
// ============================================================================

namespace AppConfig {
    // -------------------------------------------------------------------------
    // Timing Constants
    // -------------------------------------------------------------------------
    constexpr uint32_t BOOT_DURATION_MS = 3000;        // Boot screen duration
    constexpr uint32_t MENU_TIMEOUT_MS = 30000;        // Auto-return from menu
    constexpr uint32_t DISPLAY_UPDATE_MS = 100;        // Main screen refresh rate
    constexpr uint32_t ENCODER_LONG_PRESS_MS = 800;    // Long press threshold

    // -------------------------------------------------------------------------
    // Safety Thresholds
    // -------------------------------------------------------------------------
    constexpr uint8_t TEMP_WARNING_C = 60;             // Temperature warning threshold
    constexpr uint8_t TEMP_SHUTDOWN_C = 80;            // Temperature shutdown threshold
    constexpr uint32_t MIN_VBUS_FOR_17V_MV = 18000;    // Minimum VBUS for 17V buck enable

    // -------------------------------------------------------------------------
    // Current Limit Settings
    // -------------------------------------------------------------------------
    constexpr uint32_t CURRENT_LIMIT_MIN_MA = 100;     // Minimum current limit
    constexpr uint32_t CURRENT_LIMIT_MAX_MA = 5000;    // Maximum current limit
    constexpr uint32_t CURRENT_LIMIT_STEP_MA = 100;    // Adjustment step size
    constexpr uint32_t CURRENT_LIMIT_DEFAULT_MA = 1000; // Default current limit

    // -------------------------------------------------------------------------
    // Display Settings
    // -------------------------------------------------------------------------
    constexpr uint8_t LCD_BRIGHTNESS_DEFAULT = 100;    // Default brightness (%)
    constexpr uint16_t LCD_WIDTH = 240;
    constexpr uint16_t LCD_HEIGHT = 320;
}
