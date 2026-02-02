#include "display_manager.h"
#include "hardware.h"
#include "logic/state_machine.h"
#include "logic/eeprom_workflow.h"
#include "logic/safety.h"
#include "logic/pd_manager.h"
#include "logic/settings.h"
#include "config/version.h"
#include "config/app_config.h"
#include <cstdio>
#include <cstring>
#include "ui/assets/synapticon_logo.h"
#include "ui/font_config.h"

// Global instance
DisplayManager displayManager;

// Screen dimensions
static constexpr int SCREEN_WIDTH = 240;
static constexpr int SCREEN_HEIGHT = 320;

// Layout constants
static constexpr int HEADER_HEIGHT = 40;
static constexpr int STATUS_BAR_HEIGHT = 20;
static constexpr int CONTENT_Y_START = HEADER_HEIGHT + 5;
static constexpr int MENU_ITEM_HEIGHT = 25;
static constexpr int MARGIN = 10;

// ============================================================================
// Constructor
// ============================================================================

DisplayManager::DisplayManager()
    : _needs_full_redraw(true)
    , _last_rendered_state(AppState::BOOT)
    , _pdo_list(nullptr)
    , _pdo_count(0)
    , _last_menu_selection(-1)
    , _last_settings_selection(-1)
    , _last_pdo_selection(-1)
    , _last_adjust_value(0)
    , _last_pps_voltage(0)
    , _last_pps_state(-1)
    , _last_brightness_value(255)
    , _last_boot_message(nullptr)
    , _backlight_on(false)
{
}

// ============================================================================
// Initialization
// ============================================================================

void DisplayManager::init() {
    clearScreen();
    _needs_full_redraw = true;
    _last_pps_state = -1;  // Force PPS badge redraw on first render
}

// ============================================================================
// Main Render
// ============================================================================

void DisplayManager::render() {
    AppState current_state = stateMachine.getState();

    // Check if state changed - need full redraw
    if (current_state != _last_rendered_state) {
        _needs_full_redraw = true;
        _last_rendered_state = current_state;
    }

    // Clear screen on full redraw
    if (_needs_full_redraw) {
        clearScreen();
    }

    // Render based on current state
    switch (current_state) {
        case AppState::BOOT:
            renderBootScreen();
            break;

        case AppState::MAIN:
            renderMainScreen();
            break;

        case AppState::MENU:
            renderMenuScreen();
            break;

        case AppState::ADJUST:
            renderAdjustScreen();
            break;

        case AppState::FAULT:
            renderFaultScreen();
            break;
    }

    _needs_full_redraw = false;

    // Turn on backlight after first frame is fully rendered (prevents ghost image)
    // Use saved brightness level from settings
    if (!_backlight_on) {
        hw.display.setBacklightBrightness(settings.getLcdBrightness());
        _backlight_on = true;
    }
}

void DisplayManager::invalidate() {
    _needs_full_redraw = true;
}

void DisplayManager::setPdoList(const SourceCapability* pdos, uint8_t count) {
    _pdo_list = pdos;
    _pdo_count = count;
}

// ============================================================================
// Screen Renderers
// ============================================================================

void DisplayManager::renderBootScreen() {
    if (_needs_full_redraw) {
        // Draw scaled version of logo (centered)
        int logo_size = 130;
        int logo_x = (SCREEN_WIDTH - logo_size) / 2;
        int y = CONTENT_Y_START;
        hw.display.drawBitmapScaled(logo_x, y, logo_size, logo_size,
                                    SYNAPTICON_WIDTH, SYNAPTICON_HEIGHT, synapticon_data);
    }

    // Update boot text and progress
    drawBootText();
    drawBootProgress();
}

void DisplayManager::renderMainScreen() {
    if (_needs_full_redraw) {
        drawHeader("PD240W");
    }

    // Draw power readings
    drawActiveContract();
    drawPowerReadings();
    drawTemperature();
    drawOutputStatus();
}

void DisplayManager::renderMenuScreen() {
    if (_needs_full_redraw) {
        drawHeader("Menu");
    }

    MenuItem selected = stateMachine.getSelectedMenuItem();
    int8_t sel_idx = static_cast<int8_t>(selected);

    // Only redraw items when selection changed or full redraw needed
    if (_needs_full_redraw || sel_idx != _last_menu_selection) {
        int y = CONTENT_Y_START + 10;

        drawMenuItem(y, "Select Voltage", selected == MenuItem::SELECT_VOLTAGE);
        y += MENU_ITEM_HEIGHT;

        drawMenuItem(y, "Current Limit", selected == MenuItem::CURRENT_LIMIT);
        y += MENU_ITEM_HEIGHT;

        drawMenuItem(y, "Settings", selected == MenuItem::SETTINGS);
        y += MENU_ITEM_HEIGHT;

        drawMenuItem(y, "About", selected == MenuItem::ABOUT);
        y += MENU_ITEM_HEIGHT;

        drawMenuItemMuted(y, "Back", selected == MenuItem::BACK);

        _last_menu_selection = sel_idx;
    }

    // Draw hint only on full redraw
    if (_needs_full_redraw) {
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                              "Click: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
}

void DisplayManager::renderAdjustScreen() {
    AdjustMode mode = stateMachine.getAdjustMode();

    if (mode == AdjustMode::PDO_SELECT) {
        if (_needs_full_redraw) {
            drawHeader("Select Voltage");
        }
        drawPdoList();
    } else if (mode == AdjustMode::CURRENT_LIMIT) {
        if (_needs_full_redraw) {
            drawHeader("Current Limit");
        }
        drawCurrentLimitAdjust();
    } else if (mode == AdjustMode::PPS_VOLTAGE) {
        if (_needs_full_redraw) {
            drawHeader("PPS Voltage");
        }
        drawPpsVoltageAdjust();
    } else if (mode == AdjustMode::EEPROM_FLASH) {
        if (_needs_full_redraw) {
            drawHeader("Flash EEPROM");
        }
        drawEepromFlashScreen();
    } else if (mode == AdjustMode::ABOUT) {
        if (_needs_full_redraw) {
            drawHeader("About");
            drawAboutScreen();
        }
    } else if (mode == AdjustMode::SETTINGS_MENU) {
        if (_needs_full_redraw) {
            drawHeader("Settings");
        }
        drawSettingsMenu();
    }
}

void DisplayManager::renderFaultScreen() {
    if (_needs_full_redraw) {
        clearScreen();
        drawFaultIcon();
        drawFaultDetails();
    }

    // Live-update current temperature for overtemperature faults
    if (stateMachine.getFaultType() == FaultType::OVERTEMPERATURE) {
        drawFaultLiveTemperature();
    }
}

// ============================================================================
// Common UI Elements
// ============================================================================

void DisplayManager::drawHeader(const char* title) {
    // Clear header area
    hw.display.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, UIColors::BACKGROUND);

    // Draw title centered using AA font
    drawCenteredStringAA(10, title, UIColors::TEXT_PRIMARY, FONT_MEDIUM);

    // Draw separator line
    hw.display.drawLine(MARGIN, HEADER_HEIGHT - 2, SCREEN_WIDTH - MARGIN, HEADER_HEIGHT - 2, UIColors::HEADER_LINE);
}

void DisplayManager::drawProgressBar(int x, int y, int width, int height, uint8_t percent, uint16_t color) {
    // Draw border
    hw.display.drawRect(x, y, width, height, UIColors::TEXT_SECONDARY);

    // Calculate fill width
    int fill_width = ((width - 4) * percent) / 100;
    if (fill_width > 0) {
        hw.display.fillRect(x + 2, y + 2, fill_width, height - 4, color);
    }

    // Clear unfilled area
    int unfilled_start = x + 2 + fill_width;
    int unfilled_width = (width - 4) - fill_width;
    if (unfilled_width > 0) {
        hw.display.fillRect(unfilled_start, y + 2, unfilled_width, height - 4, UIColors::BACKGROUND);
    }
}

// ============================================================================
// Boot Screen Elements
// ============================================================================

void DisplayManager::drawBootText() {
    // Static text below the 220x220 logo (logo occupies y=5 to y=225)
    if (_needs_full_redraw) {
        // Product name
        drawCenteredStringAA(200, Version::PRODUCT_NAME, UIColors::TEXT_PRIMARY, FONT_MEDIUM);

        // Version
        drawCenteredStringAA(225, Version::FIRMWARE_VERSION, UIColors::TEXT_SECONDARY, FONT_SMALL);
    }

    // Stage message - only redraw when message changes (prevents flicker)
    const char* stage_msg = stateMachine.getBootStageMessage();
    if (stage_msg != _last_boot_message) {
        hw.display.fillRect(0, 255, SCREEN_WIDTH, 16, UIColors::BACKGROUND);
        if (stage_msg && stage_msg[0] != '\0') {
            drawCenteredStringAA(255, stage_msg, UIColors::TEXT_PRIMARY, FONT_SMALL);
        }
        _last_boot_message = stage_msg;
    }
}

void DisplayManager::drawBootProgress() {
    uint8_t progress = stateMachine.getBootProgress();
    drawProgressBar(MARGIN * 3, 282, SCREEN_WIDTH - MARGIN * 6, 15, progress, UIColors::SYNAPTICON_PINK);
}

// ============================================================================
// Main Screen Elements
// ============================================================================

void DisplayManager::drawActiveContract() {
    int y = CONTENT_Y_START + 5;

    // Only clear on full redraw
    if (_needs_full_redraw) {
        hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, 50, UIColors::BACKGROUND);
        hw.display.drawStringAA(MARGIN, y, "Contract:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        _last_pps_state = -1;  // Force redraw of badge
    }

    // Get active contract
    const ActiveContract& contract = pdManager.getActiveContract();

    // Use fixed-width format to avoid clearing
    char line1[32];
    if (contract.valid && contract.voltage_mv > 0) {
        snprintf(line1, sizeof(line1), "%5.2fV @ %5.2fA  ",
                 contract.voltage_mv / 1000.0f,
                 contract.current_ma / 1000.0f);
        hw.display.drawStringAA(MARGIN, y + 16, line1, UIColors::ACCENT, UIColors::BACKGROUND, FONT_MEDIUM);

        // Show PPS indicator if active - only redraw when state changes
        if (contract.is_pps) {
            if (_last_pps_state != 1) {
                // Draw "PPS" rounded badge in accent color
                int badge_x = SCREEN_WIDTH - MARGIN - 36;
                int badge_y = y - 2;
                int badge_w = 40;
                int badge_h = 18;
                int badge_r = 4;  // Corner radius
                // Draw filled rounded rectangle
                hw.display.fillRoundRect(badge_x, badge_y, badge_w, badge_h, badge_r, UIColors::ACCENT);
                // Draw text centered in badge
                int text_x = badge_x + (badge_w - ST7789::getStringWidthAA("PPS", FONT_SMALL)) / 2;
                int text_y = badge_y + (badge_h - FONT_SMALL->lineHeight) / 2;
                hw.display.drawStringAA(text_x, text_y, "PPS", UIColors::BACKGROUND, UIColors::ACCENT, FONT_SMALL);
                _last_pps_state = 1;
            }
        } else {
            if (_last_pps_state != 0) {
                // Clear PPS badge area when not in PPS mode
                hw.display.fillRect(SCREEN_WIDTH - MARGIN - 38, y - 3, 42, 22, UIColors::BACKGROUND);
                _last_pps_state = 0;
            }
        }
    } else {
        // Non-PD charger or no contract: show USB default
        hw.display.drawStringAA(MARGIN, y + 16, "USB 5V (no PD)    ", UIColors::MUTED, UIColors::BACKGROUND, FONT_MEDIUM);
        // Clear PPS badge area
        if (_last_pps_state != 0) {
            hw.display.fillRect(SCREEN_WIDTH - MARGIN - 38, y - 3, 42, 22, UIColors::BACKGROUND);
            _last_pps_state = 0;
        }
    }
}

void DisplayManager::drawPowerReadings() {
    // Power readings frame constants
    const int FRAME_X = MARGIN - 2;
    const int FRAME_Y = CONTENT_Y_START + 45;
    const int FRAME_W = SCREEN_WIDTH - 2 * MARGIN + 4;
    const int FRAME_H = 138;  // Increased to fully contain power section
    const int FRAME_R = 6;  // Corner radius

    // Draw rounded frame on full redraw
    if (_needs_full_redraw) {
        hw.display.drawRoundRect(FRAME_X, FRAME_Y, FRAME_W, FRAME_H, FRAME_R, UIColors::HEADER_LINE);
    }

    // Power readings start inside the frame
    int y = FRAME_Y + 8;

    // Layout Constants
    const int LABEL_X = MARGIN + 3;
    const int VALUE_X = MARGIN + 43; // Align all big numbers here
    const int UNIT_X  = 165;         // Fixed X for unit letters (V, A, W) — prevents shifting

    const SafetyState& state = safety.getState();
    char buf[32];

    // --- Voltage Section ---
    // Primary: Output Voltage (number and unit rendered separately for stable layout)
    hw.display.drawStringAA(LABEL_X, y + 8, "Vout", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    snprintf(buf, sizeof(buf), "%.3f", state.ina_voltage_v);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);
    int num_w = ST7789::getStringWidthAA(buf, FONT_LARGE);
    if (VALUE_X + num_w < UNIT_X)
        hw.display.fillRect(VALUE_X + num_w, y, UNIT_X - VALUE_X - num_w, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(UNIT_X, y, "V", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);

    // Secondary: Input Voltage - right aligned in frame
    y += 32;
    snprintf(buf, sizeof(buf), "Vin: %.2f V", state.vbus_voltage_v);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);

    // --- Current Section ---
    y += 14; // Gap between sections

    // Primary: Output Current (number and unit rendered separately)
    hw.display.drawStringAA(LABEL_X, y + 8, "Iout", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    snprintf(buf, sizeof(buf), "%.3f", state.current_a);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);
    num_w = ST7789::getStringWidthAA(buf, FONT_LARGE);
    if (VALUE_X + num_w < UNIT_X)
        hw.display.fillRect(VALUE_X + num_w, y, UNIT_X - VALUE_X - num_w, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(UNIT_X, y, "A", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);

    // Secondary: Current Limit
    y += 32;
    float limit_a = stateMachine.getCurrentLimitMa() / 1000.0f;
    snprintf(buf, sizeof(buf), "Lim: %.2f A", limit_a);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);

    // --- Power Section ---
    y += 14; // Gap between sections

    // Primary: Power (number and unit rendered separately)
    hw.display.drawStringAA(LABEL_X, y + 8, "Pwr", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    snprintf(buf, sizeof(buf), "%.2f", state.power_w);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);
    num_w = ST7789::getStringWidthAA(buf, FONT_LARGE);
    if (VALUE_X + num_w < UNIT_X)
        hw.display.fillRect(VALUE_X + num_w, y, UNIT_X - VALUE_X - num_w, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(UNIT_X, y, "W", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);
}

void DisplayManager::drawTemperature() {
    // Position below the power readings frame (frame ends at CONTENT_Y_START + 45 + 138 = 183)
    int y = CONTENT_Y_START + 188;
    const SafetyState& state = safety.getState();

    // Static tracking for flicker prevention
    static float last_ntc_temp = -999.0f;
    static float last_ina_temp = -999.0f;
    static bool last_blink_hide = false;

    // 1. Determine the dynamic color for the values
    uint16_t val_color = UIColors::TEXT_PRIMARY;
    bool blink_hide = false;

    if (state.temp_status == SafetyStatus::CAUTION) {
        val_color = UIColors::CAUTION;
    } else if (state.temp_status == SafetyStatus::WARNING) {
        val_color = UIColors::WARNING;
    } else if (state.temp_status == SafetyStatus::FAULT) {
        val_color = UIColors::ERROR;
    }

    // Blinking logic (only affects val_color)
    if (state.max_temperature_c > AppConfig::TEMP_CRITICAL_WARNING_C) {
        uint32_t ms = to_ms_since_boot(get_absolute_time());
        // Blink fast (250ms interval) & make a beeping sound
        if ((ms / 250) % 2 == 1) {
            blink_hide = true;
        }
    }

    // Only redraw if values or blink state changed
    bool ntc_changed = (state.temperature_c != last_ntc_temp) || (blink_hide != last_blink_hide) || _needs_full_redraw;
    bool ina_changed = (state.ina_temperature_c != last_ina_temp) || (blink_hide != last_blink_hide) || _needs_full_redraw;

    // 2. Draw using AA font with fixed X positions to prevent flicker
    char buf[16];

    // Fixed layout positions (avoids shifting when values change width)
    const int NTC_LABEL_X = MARGIN;
    const int NTC_VALUE_X = MARGIN + 32;
    const int NTC_UNIT_X = NTC_VALUE_X + 42;  // Fixed position for "C"
    const int INA_LABEL_X = 125;
    const int INA_VALUE_X = INA_LABEL_X + 32;
    const int INA_UNIT_X = INA_VALUE_X + 42;  // Fixed position for "C"
    const int VALUE_WIDTH = 40;  // Width for numeric value only

    // --- NTC temperature ---
    if (_needs_full_redraw) {
        hw.display.drawStringAA(NTC_LABEL_X, y, "NTC:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        // Draw degree symbol as small 'o' + 'C' (no extended ASCII in font)
        hw.display.drawStringAA(NTC_UNIT_X, y - 3, "o", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(NTC_UNIT_X + 6, y, "C", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
    if (ntc_changed) {
        if (blink_hide) {
            hw.display.fillRect(NTC_VALUE_X, y, VALUE_WIDTH, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
        } else {
            snprintf(buf, sizeof(buf), "%5.1f", state.temperature_c);
            hw.display.drawStringAA(NTC_VALUE_X, y, buf, val_color, UIColors::BACKGROUND, FONT_SMALL);
        }
        last_ntc_temp = state.temperature_c;
    }

    // --- INA temperature ---
    if (_needs_full_redraw) {
        hw.display.drawStringAA(INA_LABEL_X, y, "INA:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        // Draw degree symbol as small 'o' + 'C' (no extended ASCII in font)
        hw.display.drawStringAA(INA_UNIT_X, y - 3, "o", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(INA_UNIT_X + 6, y, "C", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
    if (ina_changed) {
        if (blink_hide) {
            hw.display.fillRect(INA_VALUE_X, y, VALUE_WIDTH, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
        } else {
            snprintf(buf, sizeof(buf), "%5.1f", state.ina_temperature_c);
            hw.display.drawStringAA(INA_VALUE_X, y, buf, val_color, UIColors::BACKGROUND, FONT_SMALL);
        }
        last_ina_temp = state.ina_temperature_c;
    }

    last_blink_hide = blink_hide;
}

void DisplayManager::drawOutputStatus() {
    // Position below temperature readings
    int y = CONTENT_Y_START + 208;

    // Badge constants
    const int BADGE_W = 32;
    const int BADGE_H = 16;
    const int BADGE_R = 3;
    const int BADGE_X = SCREEN_WIDTH - MARGIN - BADGE_W;  // Right-aligned badges

    // Static tracking for flicker prevention
    static bool last_load_on = false;
    static bool last_buck_on = false;

    bool load_on = hw.loadSwitch.read();
    bool buck_on = hw.EN_17V.read();

    // Only draw labels on full redraw
    if (_needs_full_redraw) {
        hw.display.drawStringAA(MARGIN, y + 2, "Load Switch:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(MARGIN, y + 22, "17V Buck:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20, "Click: Menu", UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
        // Force badge redraw
        last_load_on = !load_on;
        last_buck_on = !buck_on;
    }

    // --- Load Switch Badge ---
    if (load_on != last_load_on || _needs_full_redraw) {
        // Clear badge area first to avoid corner artifacts (+1px margin for rounding)
        hw.display.fillRect(BADGE_X - 1, y - 1, BADGE_W + 2, BADGE_H + 2, UIColors::BACKGROUND);
        if (load_on) {
            // Green "ON" badge
            hw.display.fillRoundRect(BADGE_X, y, BADGE_W, BADGE_H, BADGE_R, UIColors::ACCENT);
            int text_w = ST7789::getStringWidthAA("ON", FONT_SMALL);
            int text_x = BADGE_X + (BADGE_W - text_w) / 2;
            int text_y = y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(text_x, text_y, "ON", UIColors::BACKGROUND, UIColors::ACCENT, FONT_SMALL);
        } else {
            // Red "OFF" badge
            hw.display.fillRoundRect(BADGE_X, y, BADGE_W, BADGE_H, BADGE_R, UIColors::ERROR);
            int text_w = ST7789::getStringWidthAA("OFF", FONT_SMALL);
            int text_x = BADGE_X + (BADGE_W - text_w) / 2;
            int text_y = y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(text_x, text_y, "OFF", UIColors::BACKGROUND, UIColors::ERROR, FONT_SMALL);
        }
        last_load_on = load_on;
    }

    // --- 17V Buck Badge (aligned vertically with Load badge) ---
    if (buck_on != last_buck_on || _needs_full_redraw) {
        int buck_y = y + 20;
        // Clear badge area first to avoid corner artifacts (+1px margin for rounding)
        hw.display.fillRect(BADGE_X - 1, buck_y - 1, BADGE_W + 2, BADGE_H + 2, UIColors::BACKGROUND);
        if (buck_on) {
            // Yellow "ON" badge (safety STO/SBC)
            hw.display.fillRoundRect(BADGE_X, buck_y, BADGE_W, BADGE_H, BADGE_R, UIColors::CAUTION);
            int text_w = ST7789::getStringWidthAA("ON", FONT_SMALL);
            int text_x = BADGE_X + (BADGE_W - text_w) / 2;
            int text_y = buck_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(text_x, text_y, "ON", UIColors::BACKGROUND, UIColors::CAUTION, FONT_SMALL);
        } else {
            // Gray "OFF" badge
            hw.display.fillRoundRect(BADGE_X, buck_y, BADGE_W, BADGE_H, BADGE_R, UIColors::MUTED);
            int text_w = ST7789::getStringWidthAA("OFF", FONT_SMALL);
            int text_x = BADGE_X + (BADGE_W - text_w) / 2;
            int text_y = buck_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(text_x, text_y, "OFF", UIColors::BACKGROUND, UIColors::MUTED, FONT_SMALL);
        }
        last_buck_on = buck_on;
    }
}

// ============================================================================
// Menu Elements
// ============================================================================

void DisplayManager::drawMenuItem(int y, const char* text, bool selected) {
    uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
    uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

    // Single fill with correct background (avoids flicker from clear+highlight)
    hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);

    hw.display.drawStringAA(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, FONT_SMALL);
    hw.display.drawStringAA(MARGIN + 20, y + 5, text, fg, bg, FONT_SMALL);
}

void DisplayManager::drawMenuItemMuted(int y, const char* text, bool selected) {
    uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
    uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::MUTED;

    // Single fill with correct background (avoids flicker from clear+highlight)
    hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);

    hw.display.drawStringAA(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, FONT_SMALL);
    hw.display.drawStringAA(MARGIN + 20, y + 5, text, fg, bg, FONT_SMALL);
}

void DisplayManager::drawPdoList() {
    int8_t selected_idx = stateMachine.getSelectedPdoIndex();

    // Skip redraw if selection hasn't changed
    if (!_needs_full_redraw && selected_idx == _last_pdo_selection) {
        return;
    }
    _last_pdo_selection = selected_idx;

    int y = CONTENT_Y_START + 10;

    // Get PDO list from PD manager
    SourceCapability pdos[13];
    uint8_t count = pdManager.getSourceCapabilities(pdos, 13);

    // No contracts found - show informational message
    if (count == 0) {
        drawCenteredStringAA(y + 30, "No PD contracts", UIColors::WARNING, FONT_MEDIUM);
        drawCenteredStringAA(y + 60, "The connected charger may", UIColors::TEXT_SECONDARY, FONT_SMALL);
        drawCenteredStringAA(y + 78, "not support USB Power Delivery.", UIColors::TEXT_SECONDARY, FONT_SMALL);
        drawCenteredStringAA(y + 105, "Try a USB-C PD charger.", UIColors::MUTED, FONT_SMALL);

        if (_needs_full_redraw) {
            hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                                  "Click: Back", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        }
        return;
    }

    // Draw visible PDOs (max 8 visible at once)
    int start_idx = 0;
    if (selected_idx > 5 && count > 8) {
        start_idx = selected_idx - 5;
        if (start_idx + 8 > count) {
            start_idx = count - 8;
        }
    }

    int visible_count = (count - start_idx > 8) ? 8 : (count - start_idx);

    for (int i = start_idx; i < start_idx + visible_count; i++) {
        bool selected = (i == selected_idx);

        uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
        uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

        // Single fill with correct background (avoids flicker)
        hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);

        char line[40];
        if (pdos[i].is_pps) {
            snprintf(line, sizeof(line), "PPS %u-%uV %umA",
                     (unsigned)(pdos[i].min_voltage_mv / 1000),
                     (unsigned)(pdos[i].voltage_mv / 1000),
                     (unsigned)pdos[i].max_current_ma);
        } else if (pdos[i].is_avs) {
            snprintf(line, sizeof(line), "AVS %u-%uV %umA",
                     (unsigned)(pdos[i].min_voltage_mv / 1000),
                     (unsigned)(pdos[i].voltage_mv / 1000),
                     (unsigned)pdos[i].max_current_ma);
        } else {
            snprintf(line, sizeof(line), "%uV @ %umA",
                     (unsigned)(pdos[i].voltage_mv / 1000),
                     (unsigned)pdos[i].max_current_ma);
        }

        hw.display.drawStringAA(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, FONT_SMALL);
        hw.display.drawStringAA(MARGIN + 20, y + 5, line, fg, bg, FONT_SMALL);

        y += MENU_ITEM_HEIGHT;
    }

    // Clear remaining slots if less than 8 visible
    for (int i = visible_count; i < 8; i++) {
        hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, UIColors::BACKGROUND);
        y += MENU_ITEM_HEIGHT;
    }

    // Draw hint only on full redraw
    if (_needs_full_redraw) {
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                              "Click: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
}

void DisplayManager::drawCurrentLimitAdjust() {
    uint32_t current_ma = stateMachine.getCurrentLimitMa();
    uint32_t max_ma = stateMachine.getEffectiveMaxCurrentMa();

    // Skip redraw if value hasn't changed
    if (!_needs_full_redraw && current_ma == _last_adjust_value) {
        return;
    }
    _last_adjust_value = current_ma;

    int y = CONTENT_Y_START + 40;

    // Only clear content area on full redraw
    if (_needs_full_redraw) {
        hw.display.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START - 40, UIColors::BACKGROUND);
    }

    // Draw current value with A unit at fixed position
    // Value right-aligned, A at fixed position for stable layout
    char buf[32];
    snprintf(buf, sizeof(buf), "%5.2f", current_ma / 1000.0f);
    
    // Fixed layout: center point at screen middle, A after the number area
    const int UNIT_X = (SCREEN_WIDTH / 2) + 42;  // Fixed position for "A"
    const int VALUE_RIGHT = UNIT_X - 8;  // Right edge of value area
    int value_width = ST7789::getStringWidthAA(buf, FONT_LARGE);
    int value_x = VALUE_RIGHT - value_width;
    
    // Clear value area and redraw
    hw.display.fillRect(value_x - 20, y, VALUE_RIGHT - value_x + 20, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(value_x, y, buf, UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);
    hw.display.drawStringAA(UNIT_X, y, "A", UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);

    // Draw progress bar (scaled to effective max)
    y += 50;
    uint32_t range = max_ma - AppConfig::CURRENT_LIMIT_MIN_MA;
    uint8_t percent = (range > 0)
        ? ((current_ma - AppConfig::CURRENT_LIMIT_MIN_MA) * 100) / range
        : 0;
    drawProgressBar(MARGIN * 2, y, SCREEN_WIDTH - MARGIN * 4, 20, percent, UIColors::SYNAPTICON_PINK);

    // Draw min/max labels and hints only on full redraw
    if (_needs_full_redraw) {
        y += 30;
        char min_str[16], max_str[16];
        snprintf(min_str, sizeof(min_str), "%.1fA", AppConfig::CURRENT_LIMIT_MIN_MA / 1000.0f);
        snprintf(max_str, sizeof(max_str), "%.1fA (max)", max_ma / 1000.0f);

        hw.display.drawStringAA(MARGIN * 2, y, min_str, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);

        // Right-align max
        int max_width = ST7789::getStringWidthAA(max_str, FONT_SMALL);
        hw.display.drawStringAA(SCREEN_WIDTH - MARGIN * 2 - max_width, y, max_str,
                              UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);

        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 35,
                              "Rotate: Adjust", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                              "Click: Confirm", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
}

void DisplayManager::drawPpsVoltageAdjust() {
    uint32_t target_mv = stateMachine.getPpsTargetVoltageMv();
    uint32_t min_mv = stateMachine.getPpsMinVoltageMv();
    uint32_t max_mv = stateMachine.getPpsMaxVoltageMv();
    uint32_t max_current = stateMachine.getPpsMaxCurrentMa();

    // Skip redraw if value hasn't changed
    if (!_needs_full_redraw && target_mv == _last_pps_voltage) {
        return;
    }
    _last_pps_voltage = target_mv;

    int y = CONTENT_Y_START + 20;

    // Only clear content area on full redraw
    if (_needs_full_redraw) {
        hw.display.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START - 40, UIColors::BACKGROUND);

        // Draw "PPS" badge
        drawCenteredStringAA(y, "Programmable Power", UIColors::SYNAPTICON_PINK, FONT_SMALL);
        y += 18;
    } else {
        y += 18;
    }

    // Draw target voltage - large display with V at fixed position
    y += 10;
    char buf[32];
    snprintf(buf, sizeof(buf), "%6.3f", target_mv / 1000.0f);
    
    // Fixed layout: center point at screen middle, V after the number area
    const int UNIT_X = (SCREEN_WIDTH / 2) + 52;  // Fixed position for "V" (adjusted for 3 decimals)
    const int VALUE_RIGHT = UNIT_X - 8;  // Right edge of value area
    int value_width = ST7789::getStringWidthAA(buf, FONT_LARGE);
    int value_x = VALUE_RIGHT - value_width;
    
    // Clear value area and redraw
    hw.display.fillRect(value_x - 20, y, VALUE_RIGHT - value_x + 20, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(value_x, y, buf, UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);
    hw.display.drawStringAA(UNIT_X, y, "V", UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);

    // Draw progress bar (scaled to PPS range)
    y += 50;
    uint32_t range = max_mv - min_mv;
    uint8_t percent = (range > 0)
        ? ((target_mv - min_mv) * 100) / range
        : 0;
    drawProgressBar(MARGIN * 2, y, SCREEN_WIDTH - MARGIN * 4, 20, percent, UIColors::SYNAPTICON_PINK);

    // Draw min/max labels
    if (_needs_full_redraw) {
        y += 30;
        char min_str[16], max_str[16];
        snprintf(min_str, sizeof(min_str), "%.1fV", min_mv / 1000.0f);
        snprintf(max_str, sizeof(max_str), "%.1fV", max_mv / 1000.0f);

        hw.display.drawStringAA(MARGIN * 2, y, min_str, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);

        // Right-align max
        int max_width = ST7789::getStringWidthAA(max_str, FONT_SMALL);
        hw.display.drawStringAA(SCREEN_WIDTH - MARGIN * 2 - max_width, y, max_str,
                              UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);

        // Show max current available
        y += 25;
        snprintf(buf, sizeof(buf), "Max current: %umA", (unsigned)max_current);
        drawCenteredStringAA(y, buf, UIColors::TEXT_SECONDARY, FONT_SMALL);

        // Hints
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 35,
                              "Rotate: 20mV steps", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                              "Click: Confirm", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
}

// ============================================================================
// Settings Menu
// ============================================================================

void DisplayManager::drawSettingsMenu() {
    SettingsItem selected = stateMachine.getSelectedSettingsItem();
    int8_t sel_idx = static_cast<int8_t>(selected);
    bool brightness_adjusting = stateMachine.isBrightnessAdjusting();
    static bool last_brightness_adjusting = false;
    static uint8_t last_brightness_value = 100;
    uint8_t current_brightness = stateMachine.getBrightnessValue();

    // Check if only brightness value changed (no selection change)
    bool brightness_only_update = !_needs_full_redraw && 
                                  sel_idx == _last_settings_selection &&
                                  selected == SettingsItem::BRIGHTNESS &&
                                  (brightness_adjusting != last_brightness_adjusting ||
                                   current_brightness != last_brightness_value);

    // Only redraw items when selection changed or full redraw needed
    if (_needs_full_redraw || sel_idx != _last_settings_selection || brightness_only_update) {
        int y = CONTENT_Y_START + 10;

        if (!brightness_only_update) {
            // Flash EEPROM - regular menu item
            drawMenuItem(y, "Flash EEPROM", selected == SettingsItem::FLASH_EEPROM);
        }
        y += MENU_ITEM_HEIGHT;

        if (!brightness_only_update) {
            // Auto PPS - ON/OFF toggle
            drawSettingsItem(y, "Auto PPS tuning", settings.isAutoPpsEnabled(), 
                            selected == SettingsItem::AUTO_PPS, true);
        }
        y += MENU_ITEM_HEIGHT;

        // Brightness - always redraw if brightness_only_update or selection changed
        drawBrightnessItem(y, selected == SettingsItem::BRIGHTNESS);
        y += MENU_ITEM_HEIGHT;

        if (!brightness_only_update) {
            // Sounds - ON/OFF toggle
            drawSettingsItem(y, "Sounds", settings.isSoundsEnabled(),
                            selected == SettingsItem::SOUNDS, true);
            y += MENU_ITEM_HEIGHT;

            // Back - muted color
            drawMenuItemMuted(y, "Back", selected == SettingsItem::BACK);
        }

        _last_settings_selection = sel_idx;
        last_brightness_adjusting = brightness_adjusting;
        last_brightness_value = current_brightness;
    }

    // Draw hint only on full redraw
    if (_needs_full_redraw) {
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                              "Click: Toggle/Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
}

void DisplayManager::drawSettingsItem(int y, const char* label, bool is_on, bool selected, bool is_toggle) {
    uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
    uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

    // Single fill with correct background
    hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);

    // Draw selection cursor
    hw.display.drawStringAA(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, FONT_SMALL);
    
    // Draw label
    hw.display.drawStringAA(MARGIN + 20, y + 5, label, fg, bg, FONT_SMALL);

    if (is_toggle) {
        // Draw ON/OFF badge on the right
        const int BADGE_W = 32;
        const int BADGE_H = 16;
        const int BADGE_R = 3;
        const int BADGE_X = SCREEN_WIDTH - MARGIN - BADGE_W - 5;
        const int badge_y = y + (MENU_ITEM_HEIGHT - BADGE_H) / 2 - 1;

        if (is_on) {
            // Green "ON" badge
            hw.display.fillRoundRect(BADGE_X, badge_y, BADGE_W, BADGE_H, BADGE_R, UIColors::ACCENT);
            int text_w = ST7789::getStringWidthAA("ON", FONT_SMALL);
            int text_x = BADGE_X + (BADGE_W - text_w) / 2;
            int text_y = badge_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(text_x, text_y, "ON", UIColors::BACKGROUND, UIColors::ACCENT, FONT_SMALL);
        } else {
            // Muted "OFF" badge
            hw.display.fillRoundRect(BADGE_X, badge_y, BADGE_W, BADGE_H, BADGE_R, UIColors::MUTED);
            int text_w = ST7789::getStringWidthAA("OFF", FONT_SMALL);
            int text_x = BADGE_X + (BADGE_W - text_w) / 2;
            int text_y = badge_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(text_x, text_y, "OFF", UIColors::BACKGROUND, UIColors::MUTED, FONT_SMALL);
        }
    }
}

void DisplayManager::drawBrightnessItem(int y, bool selected) {
    bool adjusting = stateMachine.isBrightnessAdjusting();
    uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
    uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

    // Single fill with correct background
    hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);

    // Draw selection cursor (arrows when adjusting)
    if (selected && adjusting) {
        hw.display.drawStringAA(MARGIN + 5, y + 5, "<", fg, bg, FONT_SMALL);
    } else {
        hw.display.drawStringAA(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, FONT_SMALL);
    }
    
    // Draw label "Brightness:"
    hw.display.drawStringAA(MARGIN + 20, y + 5, "Brightness:", fg, bg, FONT_SMALL);

    // Draw brightness value on the right (use current value from stateMachine when adjusting)
    char buf[8];
    uint8_t brightness = adjusting ? stateMachine.getBrightnessValue() : settings.getLcdBrightness();
    snprintf(buf, sizeof(buf), "%3d%%", brightness);
    int value_x = SCREEN_WIDTH - MARGIN - ST7789::getStringWidthAA(buf, FONT_SMALL) - 10;
    hw.display.drawStringAA(value_x, y + 5, buf, fg, bg, FONT_SMALL);
    
    // Show > indicator on right when adjusting
    if (selected && adjusting) {
        hw.display.drawStringAA(SCREEN_WIDTH - MARGIN - 10, y + 5, ">", fg, bg, FONT_SMALL);
    }
}

void DisplayManager::drawEepromFlashScreen() {
    uint8_t stage = static_cast<uint8_t>(eepromWorkflow.getStage());
    uint8_t phase = eepromWorkflow.getPhase();
    uint8_t progress = eepromWorkflow.getProgress();
    bool result = eepromWorkflow.getResult();
    bool confirm_yes = eepromWorkflow.isConfirmYes();
    const char* message = eepromWorkflow.getMessage();

    // Clear content area on full redraw or stage change
    static uint8_t last_stage = 255;
    static uint8_t last_progress = 255;
    static bool last_confirm_yes = false;

    bool stage_changed = (stage != last_stage);
    bool progress_changed = (progress != last_progress);
    bool confirm_changed = (confirm_yes != last_confirm_yes);

    if (_needs_full_redraw || stage_changed) {
        hw.display.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START, UIColors::BACKGROUND);
        last_stage = stage;
    }

    int y = CONTENT_Y_START + 20;

    switch (stage) {
        case 0:  // Comparing (initializing)
            drawCenteredStringAA(y, "Checking EEPROM...", UIColors::TEXT_PRIMARY, FONT_MEDIUM);
            y += 35;
            drawCenteredStringAA(y, "Please wait", UIColors::TEXT_SECONDARY, FONT_SMALL);
            break;

        case 1:  // Confirm stage - show result and Yes/No
            if (message) {
                drawCenteredStringAA(y, message, UIColors::CAUTION, FONT_MEDIUM);
            }
            y += 30;

            drawCenteredStringAA(y, "Proceed with flash?", UIColors::TEXT_PRIMARY, FONT_SMALL);
            y += 35;

            // Draw Yes/No buttons
            {
                int btn_width = 80;
                int btn_height = 30;
                int spacing = 30;
                int total_width = btn_width * 2 + spacing;
                int start_x = (SCREEN_WIDTH - total_width) / 2;

                // "No" button (left)
                uint16_t no_bg = confirm_yes ? UIColors::BACKGROUND : UIColors::HIGHLIGHT_BG;
                uint16_t no_fg = confirm_yes ? UIColors::TEXT_SECONDARY : UIColors::HIGHLIGHT_FG;
                hw.display.fillRect(start_x, y, btn_width, btn_height, no_bg);
                hw.display.drawRect(start_x, y, btn_width, btn_height, UIColors::TEXT_SECONDARY);
                int no_x = start_x + (btn_width - ST7789::getStringWidthAA("No", FONT_MEDIUM)) / 2;
                hw.display.drawStringAA(no_x, y + 5, "No", no_fg, no_bg, FONT_MEDIUM);

                // "Yes" button (right)
                uint16_t yes_bg = confirm_yes ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
                uint16_t yes_fg = confirm_yes ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_SECONDARY;
                hw.display.fillRect(start_x + btn_width + spacing, y, btn_width, btn_height, yes_bg);
                hw.display.drawRect(start_x + btn_width + spacing, y, btn_width, btn_height, UIColors::TEXT_SECONDARY);
                int yes_x = start_x + btn_width + spacing + (btn_width - ST7789::getStringWidthAA("Yes", FONT_MEDIUM)) / 2;
                hw.display.drawStringAA(yes_x, y + 5, "Yes", yes_fg, yes_bg, FONT_MEDIUM);

                last_confirm_yes = confirm_yes;
            }

            hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 35,
                                  "Rotate: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
            hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                                  "Click: Confirm", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
            break;

        case 2:  // Flashing - show progress
            if (message) {
                drawCenteredStringAA(y, message, UIColors::ACCENT, FONT_MEDIUM);
            }
            y += 35;

            // Phase label
            {
                const char* phase_label = (phase == 0) ? "Writing to EEPROM..." : "Verifying...";
                drawCenteredStringAA(y, phase_label, UIColors::TEXT_SECONDARY, FONT_SMALL);
            }
            y += 25;

            // Progress bar
            if (_needs_full_redraw || progress_changed) {
                drawProgressBar(MARGIN * 2, y, SCREEN_WIDTH - MARGIN * 4, 25, progress, UIColors::SYNAPTICON_PINK);
                last_progress = progress;
            }
            y += 35;

            // Progress percentage
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d%%", progress);
                drawCenteredStringAA(y, buf, UIColors::TEXT_PRIMARY, FONT_MEDIUM);
            }

            y += 30;
            drawCenteredStringAA(y, "Do not disconnect power!", UIColors::WARNING, FONT_SMALL);
            break;

        case 3:  // Done - show result
            if (result) {
                drawCenteredStringAA(y, "Success!", UIColors::ACCENT, FONT_MEDIUM);
                y += 35;
                drawCenteredStringAA(y, "EEPROM programmed", UIColors::TEXT_PRIMARY, FONT_SMALL);
                y += 20;
                drawCenteredStringAA(y, "Power cycle the board", UIColors::CAUTION, FONT_SMALL);
                y += 18;
                drawCenteredStringAA(y, "to load new config", UIColors::CAUTION, FONT_SMALL);
            } else {
                // Check if it was "already identical"
                if (message && strstr(message, "identical")) {
                    drawCenteredStringAA(y, "Already Up-to-Date", UIColors::ACCENT, FONT_MEDIUM);
                    y += 35;
                    drawCenteredStringAA(y, "EEPROM config matches", UIColors::TEXT_SECONDARY, FONT_SMALL);
                    y += 18;
                    drawCenteredStringAA(y, "No flash needed", UIColors::TEXT_SECONDARY, FONT_SMALL);
                } else {
                    drawCenteredStringAA(y, "Failed!", UIColors::ERROR, FONT_MEDIUM);
                    y += 35;
                    if (message) {
                        drawCenteredStringAA(y, message, UIColors::TEXT_SECONDARY, FONT_SMALL);
                    }
                }
            }

            hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                                  "Click: Back", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
            break;
    }
}

// ============================================================================
// About Screen
// ============================================================================

void DisplayManager::drawAboutScreen() {
    const int LINE_H = 16;

    // Logo on left, product name on right
    int logo_size = 44;
    int logo_x = MARGIN + 5;
    int logo_y = CONTENT_Y_START + 5;
    hw.display.drawBitmapScaled(logo_x, logo_y, logo_size, logo_size,
                                 SYNAPTICON_WIDTH, SYNAPTICON_HEIGHT, synapticon_data);

    // Text to the right of logo
    int text_x = MARGIN + 73;
    int text_y = logo_y;  // Vertically center text with logo
    hw.display.drawStringAA(text_x, text_y, Version::PRODUCT_NAME, UIColors::SYNAPTICON_PINK, UIColors::BACKGROUND, FONT_MEDIUM);
    hw.display.drawStringAA(text_x, text_y + 22, Version::PRODUCT_SUBTITLE, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);

    // Start info section below logo
    int y = logo_y + logo_size + 10;

    // Separator
    hw.display.drawLine(MARGIN * 3, y, SCREEN_WIDTH - MARGIN * 3, y, UIColors::HEADER_LINE);
    y += 8;

    // Info lines — compact two-column layout to fit 240px width
    const int LABEL_X = MARGIN + 5;
    const int VALUE_X = LABEL_X + 55;
    char buf[48];

    // HW / FW on one line
    hw.display.drawStringAA(LABEL_X, y, "HW:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    hw.display.drawStringAA(VALUE_X, y, Version::HARDWARE_VERSION, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H;

    hw.display.drawStringAA(LABEL_X, y, "FW:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    hw.display.drawStringAA(VALUE_X, y, Version::FIRMWARE_VERSION, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H;

    hw.display.drawStringAA(LABEL_X, y, "Author:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    hw.display.drawStringAA(VALUE_X, y, Version::AUTHOR, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H;

    hw.display.drawStringAA(LABEL_X, y, "Built:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    hw.display.drawStringAA(VALUE_X, y, BUILD_DATE, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H;

    hw.display.drawStringAA(LABEL_X, y, "Target:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    hw.display.drawStringAA(VALUE_X, y, Version::TARGET, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H;

    hw.display.drawStringAA(LABEL_X, y, "Max:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    hw.display.drawStringAA(VALUE_X, y, "48V 5A (240W)", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H;

    // Flash usage using linker symbols (RP2040 has 2MB flash)
    // These symbols are defined by the Pico SDK linker script
    extern char __flash_binary_start;
    extern char __flash_binary_end;
    uint32_t flash_used = (uint32_t)(&__flash_binary_end - &__flash_binary_start);
    constexpr uint32_t FLASH_TOTAL = 2 * 1024 * 1024;  // 2MB RP2040 internal flash
    float flash_percent = (flash_used * 100.0f) / FLASH_TOTAL;
    hw.display.drawStringAA(LABEL_X, y, "Flash:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    snprintf(buf, sizeof(buf), "%luKB (%.1f%%)", (unsigned long)(flash_used / 1024), flash_percent);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H + 4;

    // Separator
    hw.display.drawLine(MARGIN * 3, y, SCREEN_WIDTH - MARGIN * 3, y, UIColors::HEADER_LINE);
    y += 8;

    // GitHub link section
    hw.display.drawStringAA(LABEL_X, y, "Link:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    // Display shortened URL
    hw.display.drawStringAA(VALUE_X, y, "synapticon/PD240W", UIColors::LINK_BLUE, UIColors::BACKGROUND, FONT_SMALL);
    y += LINE_H + 4;

    // Separator
    hw.display.drawLine(MARGIN * 3, y, SCREEN_WIDTH - MARGIN * 3, y, UIColors::HEADER_LINE);
    y += 8;

    drawCenteredStringAA(y, Version::COMPANY, UIColors::SYNAPTICON_PINK, FONT_SMALL);

    // Navigation hint
    hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                          "Click: Back", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
}

// ============================================================================
// Fault Screen Elements
// ============================================================================

void DisplayManager::drawFaultIcon() {
    // Draw warning triangle (simplified)
    int cx = SCREEN_WIDTH / 2;
    int cy = 80;

    // Red background
    hw.display.fillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, UIColors::BACKGROUND);

    // Draw "!" symbol using large font
    hw.display.drawString(cx - 10, cy - 20, "!", UIColors::ERROR, UIColors::BACKGROUND, 4);

    // Draw FAULT header
    drawCenteredStringAA(cy + 30, "FAULT", UIColors::ERROR, FONT_MEDIUM);
}

void DisplayManager::drawFaultDetails() {
    int y = 140;

    // Clear details area
    hw.display.fillRect(0, y, SCREEN_WIDTH, SCREEN_HEIGHT - y - 40, UIColors::BACKGROUND);

    FaultType fault = stateMachine.getFaultType();
    const char* fault_name = "";
    char detail1[32] = "";
    char detail2[32] = "";

    switch (fault) {
        case FaultType::OVERCURRENT:
            fault_name = "OVERCURRENT";
            snprintf(detail1, sizeof(detail1), "Measured: %.2fA", safety.getState().current_a);
            snprintf(detail2, sizeof(detail2), "Limit: %.2fA", 5.0f);
            break;

        case FaultType::OVERTEMPERATURE:
            fault_name = "OVERTEMPERATURE";
            snprintf(detail1, sizeof(detail1), "Trigger: %.1fC", safety.getState().max_temperature_c);
            snprintf(detail2, sizeof(detail2), "Limit: %dC", AppConfig::TEMP_SHUTDOWN_C);
            break;

        case FaultType::PD_DISCONNECT:
            fault_name = "USB-PD DISCONNECTED";
            snprintf(detail1, sizeof(detail1), "VBUS: %.1fV", safety.getState().vbus_voltage_v);
            break;

        default:
            fault_name = "UNKNOWN";
            break;
    }

    drawCenteredStringAA(y, fault_name, UIColors::ERROR, FONT_MEDIUM);

    y += 35;
    if (detail1[0]) {
        drawCenteredStringAA(y, detail1, UIColors::TEXT_PRIMARY, FONT_SMALL);
        y += 20;
    }
    if (detail2[0]) {
        drawCenteredStringAA(y, detail2, UIColors::TEXT_PRIMARY, FONT_SMALL);
        y += 20;
    }

    // Live temperature line (updated dynamically by drawFaultLiveTemperature)
    if (fault == FaultType::OVERTEMPERATURE) {
        y += 5;
        drawCenteredStringAA(y, "Now:      ", UIColors::WARNING, FONT_SMALL);
        y += 20;
    }

    y += 20;
    drawCenteredStringAA(y, "Load switch disabled", UIColors::WARNING, FONT_SMALL);

    // Draw acknowledge hint
    drawCenteredStringAA(SCREEN_HEIGHT - 30, "[Press knob to acknowledge]", UIColors::TEXT_SECONDARY, FONT_SMALL);
}

void DisplayManager::drawFaultLiveTemperature() {
    // Live temperature reading at fixed position on fault screen
    const int y = 225;
    const SafetyState& state = safety.getState();

    char buf[32];
    snprintf(buf, sizeof(buf), "Now: %5.1fC", state.max_temperature_c);

    // Use fixed-width format to overwrite previous value without clearing
    drawCenteredStringAA(y, buf, UIColors::WARNING, FONT_SMALL);
}

// ============================================================================
// Helper Functions
// ============================================================================

void DisplayManager::clearScreen() {
    hw.display.fillScreen(UIColors::BACKGROUND);
}

void DisplayManager::drawCenteredString(int y, const char* text, uint16_t color, uint8_t size) {
    // Calculate approximate width (6 pixels per char at size 1)
    int char_width = 6 * size;
    int text_width = strlen(text) * char_width;
    int x = (SCREEN_WIDTH - text_width) / 2;

    if (x < 0) x = 0;

    hw.display.drawString(x, y, text, color, UIColors::BACKGROUND, size);
}

void DisplayManager::drawCenteredStringAA(int y, const char* text, uint16_t color, const AAFont* font) {
    int text_width = ST7789::getStringWidthAA(text, font);
    int x = (SCREEN_WIDTH - text_width) / 2;
    if (x < 0) x = 0;

    hw.display.drawStringAA(x, y, text, color, UIColors::BACKGROUND, font);
}
