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
    constexpr uint16_t HIGHLIGHT_BG = 0xF81F;    // Bright Magenta, Was Pink (0xE00F)
    constexpr uint16_t HIGHLIGHT_FG = 0xFFFF;    // White
    constexpr uint16_t HEADER_LINE = 0x001F;     // Deep Blue, Was Pink (0xE00F)
    constexpr uint16_t MUTED = 0x7BEF;           // Dark gray
    constexpr uint16_t PINK = 0xF81F;            // Bright Magenta, Was Pink (0xE00F)
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
    int8_t _last_pps_state;  // -1=unknown, 0=not PPS/AVS, 1=PPS, 2=AVS
    bool _last_pd_revision_drawn;  // True if PD revision badge was drawn
    char _last_pd_revision[8];     // Last drawn PD revision string
    bool _last_epr_badge_drawn;    // True if EPR badge was drawn

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
    void drawProgressBar(int x, int y, int width, int height, uint8_t percent,
                         uint16_t start_color, uint16_t end_color = 0);

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
    void drawAvsVoltageAdjust();

    // Settings menu elements
    void drawSettingsMenu();
    void drawSettingsItem(int y, const char* label, bool is_on, bool selected, bool is_toggle);
    void drawBrightnessItem(int y, bool selected);
    void drawValueAdjustItem(int y, const char* label, const char* value, bool selected, bool adjusting);

    // EEPROM flash screen elements
    void drawEepromFlashScreen();

    // About screen elements
    void drawAboutScreen();
    void drawAboutChargerScreen();

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
    int8_t _last_pdo_scroll_idx;  // Scroll position in PDO list
    uint32_t _last_adjust_value;
    uint32_t _last_main_current_limit_ma;
    uint8_t _last_current_limit_percent;
    uint32_t _last_pps_voltage;
    uint8_t _last_pps_percent;
    uint32_t _last_avs_voltage;
    uint8_t _last_avs_percent;
    uint8_t _last_brightness_value;
    const char* _last_boot_message;
    uint8_t _last_boot_progress;

    // Settings value tracking (redraw when any setting value changes)
    bool _last_auto_pps;
    bool _last_auto_avs;
    bool _last_auto_output;
    bool _last_sounds;
    uint8_t _last_dim_timeout;
    uint8_t _last_melody;
    bool _last_brightness_adjusting;
    bool _last_dim_adjusting;
    bool _last_melody_adjusting;
    uint8_t _last_contract_mode;
    bool _last_contract_mode_adjusting;

    // PPS/AVS tuning badge tracking
    bool _last_pps_converged;
    bool _last_avs_converged;

    // Current-limit badge tracking
    int8_t _last_cc_badge_state;  // -1=unknown, 0=hidden, 1=OCP, 2=CC regulating, 3=CC idle
    int8_t _last_cc_adjust_state; // -1=unknown, 0=OFF, 1=OCP, 2=CC
    int8_t _last_main_current_limit_mode;  // -1=unknown, 0=OFF, 1=OCP, 2=CC

    // Energy display mode tracking
    int8_t _last_energy_mode;  // -1=unknown, 0=mAh, 1=mWh

    // Render-function tracking variables (member vars to avoid stale state on warm reset)
    float _last_ntc_temp;
    float _last_ina_temp;
    bool _last_blink_hide;
    bool _last_load_on;
    bool _last_buck_on;
    double _last_energy_value;
    bool _last_energy_high;
    uint8_t _last_eeprom_stage;
    uint8_t _last_eeprom_progress;
    uint8_t _last_eeprom_phase;
    bool _last_eeprom_confirm;

    // Remote mode badge tracking
    bool _last_remote_mode;

    // Overtemperature fault screen: Y position of "Now" row (set by drawFaultDetails)
    int16_t _fault_now_temp_y;
};

// Global instance
extern DisplayManager displayManager;
