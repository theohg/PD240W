#pragma once

#include <cstdint>

// ============================================================================
// Application Configuration Constants
// ============================================================================

namespace AppConfig {
    // -------------------------------------------------------------------------
    // Timing Constants
    // -------------------------------------------------------------------------
    constexpr uint32_t BOOT_DURATION_MS = 2000;        // Boot screen max duration [ms]
    constexpr uint32_t BOOT_MIN_DISPLAY_MS = 500;     // Minimum boot screen time [ms]
    constexpr uint32_t BOOT_READY_DELAY_MS = 300;     // Time to show "Ready!" before transition [ms]
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
    // AVS Voltage Settings
    // -------------------------------------------------------------------------
    constexpr uint32_t AVS_VOLTAGE_STEP_MV = 25;        // AVS voltage step size [mV] (PD spec EPR minimum)
    constexpr uint32_t AVS_VELOCITY_MULT = 4;           // Higher multiplier for wider range (15-48V)

    // -------------------------------------------------------------------------
    // Display Settings
    // -------------------------------------------------------------------------
    constexpr uint8_t LCD_BRIGHTNESS_DEFAULT = 100;    // Default brightness (%)
    constexpr uint8_t LCD_BRIGHTNESS_MIN = 5;          // Minimum brightness (%)
    constexpr uint8_t LCD_BRIGHTNESS_MAX = 100;        // Maximum brightness (%)
    constexpr uint8_t LCD_BRIGHTNESS_STEP = 5;         // Brightness adjustment step (%)
    constexpr uint8_t LCD_BRIGHTNESS_DIM = 5;          // Dimmed brightness (%)
    constexpr uint16_t LCD_WIDTH = 240;
    constexpr uint16_t LCD_HEIGHT = 320;

    // -------------------------------------------------------------------------
    // Settings Limits
    // -------------------------------------------------------------------------
    constexpr uint8_t AUTO_DIM_MIN_MINUTES = 1;        // Minimum dim timeout [min]
    constexpr uint8_t AUTO_DIM_MAX_MINUTES = 10;       // Maximum dim timeout [min]
    constexpr uint8_t STARTUP_MELODY_MAX = 3;          // Max melody index (0=Silent..3=TwoTone)
    constexpr uint8_t STARTUP_CONTRACT_MODE_MAX = 2;   // Max contract mode (0=Lowest..2=LastUsed)

    // -------------------------------------------------------------------------
    // Buzzer Tones
    // -------------------------------------------------------------------------
    constexpr uint16_t BEEP_NAV_FREQ = 800;            // Navigation beep frequency [Hz]
    constexpr uint16_t BEEP_NAV_DURATION = 20;         // Navigation beep duration [ms]
    constexpr uint16_t BEEP_SELECT_FREQ = 1400;        // Select/confirm beep frequency [Hz]
    constexpr uint16_t BEEP_SELECT_DURATION = 30;      // Select/confirm beep duration [ms]
    constexpr uint16_t BEEP_EXIT_FREQ = 1000;          // Exit/back beep frequency [Hz]
    constexpr uint16_t BEEP_EXIT_DURATION = 30;        // Exit/back beep duration [ms]
    constexpr uint16_t BEEP_CONFIRM_FREQ = 1000;       // Confirm action beep frequency [Hz]
    constexpr uint16_t BEEP_CONFIRM_DURATION = 50;     // Confirm action beep duration [ms]
    constexpr uint16_t BEEP_ERROR_FREQ = 200;          // Error beep frequency [Hz]
    constexpr uint16_t BEEP_ERROR_DURATION = 200;      // Error beep duration [ms]
    constexpr uint16_t BEEP_WARNING_DURATION = 100;    // Warning beep duration [ms]
    constexpr uint16_t BEEP_FAULT_FREQ = 1000;         // Fault alert frequency [Hz]
    constexpr uint16_t BEEP_FAULT_DURATION = 500;      // Fault alert duration [ms]

    // -------------------------------------------------------------------------
    // RGB LED Settings
    // -------------------------------------------------------------------------
    constexpr uint8_t RGB_LED_BRIGHTNESS_NORMAL = 50;  // Normal brightness (%)
    constexpr uint8_t RGB_LED_BRIGHTNESS_DIM = 2;      // Dimmed brightness (%)

    // -------------------------------------------------------------------------
    // PDO Cache
    // -------------------------------------------------------------------------
    constexpr uint8_t MAX_PDO_COUNT = 13;              // Max PDOs (7 SPR + 6 EPR per USB-PD spec)

    // -------------------------------------------------------------------------
    // Temperature Display
    // -------------------------------------------------------------------------
    constexpr uint32_t CRITICAL_BLINK_INTERVAL_MS = 250; // Blink interval for critical temp [ms]
}
