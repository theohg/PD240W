#pragma once

#include <cstdint>
#include "logic/state_machine.h"
#include "logic/safety.h"
#include "drivers/power/tps26750/tps26750.h"
#include "drivers/display/aa_font.h"

// ============================================================================
// Display Manager
// ============================================================================
// Coordinates all screen rendering based on application state.
// Each state has its own render function that draws the appropriate UI.
// ============================================================================

// Colors used throughout the UI
namespace UIColors {
    constexpr uint16_t BACKGROUND = 0x0000;      // Black
    constexpr uint16_t TEXT_PRIMARY = 0xFFFF;    // White
    constexpr uint16_t TEXT_SECONDARY = 0xC618;  // Light gray
    constexpr uint16_t ACCENT = 0x07E0;          // Green
    constexpr uint16_t CAUTION = 0xFFE0;         // Yellow
    constexpr uint16_t WARNING = 0xFD20;         // Orange
    constexpr uint16_t ERROR = 0xF800;           // Red
    constexpr uint16_t HIGHLIGHT_BG = 0x001F;    // Blue
    constexpr uint16_t HIGHLIGHT_FG = 0xFFE0;    // Yellow
    constexpr uint16_t HEADER_LINE = 0xE00F;     // Synapticon Pink
    constexpr uint16_t MUTED = 0x7BEF;           // Dark gray
    constexpr uint16_t SYNAPTICON_PINK = 0xE00F; // Brand Magenta
    constexpr uint16_t LINK_BLUE = 0x5D9F;       // Hyperlink blue
}

class DisplayManager {
public:
    DisplayManager();

    // Initialize display manager
    void init();

    // Main render function - renders based on current state
    void render();

    // Force full redraw on next render
    void invalidate();

    // Set PDO list for rendering (called by state machine)
    void setPdoList(const SourceCapability* pdos, uint8_t count);

private:
    // Render flags
    bool _needs_full_redraw;
    bool _backlight_on;
    AppState _last_rendered_state;
    int8_t _last_pps_state;  // -1=unknown, 0=not PPS, 1=PPS

    // PDO list reference
    const SourceCapability* _pdo_list;
    uint8_t _pdo_count;

    // Screen renderers
    void renderBootScreen();
    void renderMainScreen();
    void renderMenuScreen();
    void renderAdjustScreen();
    void renderFaultScreen();

    // Common UI elements
    void drawHeader(const char* title);
    void drawProgressBar(int x, int y, int width, int height, uint8_t percent, uint16_t color);

    // Boot screen elements
    void drawBootText();
    void drawBootProgress();

    // Main screen elements
    void drawActiveContract();
    void drawPowerReadings();
    void drawTemperature();
    void drawOutputStatus();

    // Menu elements
    void drawMenuItem(int y, const char* text, bool selected);
    void drawMenuItemMuted(int y, const char* text, bool selected);
    void drawPdoList();
    void drawCurrentLimitAdjust();
    void drawPpsVoltageAdjust();

    // Settings menu elements
    void drawSettingsMenu();
    void drawSettingsItem(int y, const char* label, bool is_on, bool selected, bool is_toggle);
    void drawBrightnessItem(int y, bool selected);

    // EEPROM flash screen elements
    void drawEepromFlashScreen();

    // About screen elements
    void drawAboutScreen();

    // Fault screen elements
    void drawFaultIcon();
    void drawFaultDetails();
    void drawFaultLiveTemperature();

    // Helper functions
    void clearScreen();
    void drawCenteredString(int y, const char* text, uint16_t color, uint8_t size);
    void drawCenteredStringAA(int y, const char* text, uint16_t color, const AAFont* font);

    // Tracking for flicker reduction (skip redraw when unchanged)
    int8_t _last_menu_selection;
    int8_t _last_settings_selection;
    int8_t _last_pdo_selection;
    uint32_t _last_adjust_value;
    uint32_t _last_pps_voltage;
    uint8_t _last_brightness_value;
    const char* _last_boot_message;
};

// Global instance
extern DisplayManager displayManager;
