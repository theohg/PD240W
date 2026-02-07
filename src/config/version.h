#pragma once

// ============================================================================
// Firmware Version Information
// ============================================================================

namespace Version {
    constexpr const char* FIRMWARE_VERSION = "v1.0.0";
    constexpr const char* HARDWARE_VERSION = "A.1";
    constexpr const char* PRODUCT_NAME = "PD240W";
    constexpr const char* PRODUCT_SUBTITLE = "Power Supply";
    constexpr const char* AUTHOR = "Theo Heng";
    constexpr const char* COMPANY = "Synapticon GmbH";
    constexpr const char* GITHUB = "https://github.com/theohg/PD240W";
    constexpr const char* GITHUB_SHORT = "theohg/PD240W";
    constexpr const char* TARGET = "RP2040 (Pico)";

    // Build info (can be overridden by build system)
    #ifndef BUILD_DATE
        #define BUILD_DATE __DATE__
    #endif

    #ifndef BUILD_TIME
        #define BUILD_TIME __TIME__
    #endif
}
