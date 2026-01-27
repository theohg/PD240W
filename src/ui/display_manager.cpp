#include "display_manager.h"
#include "hardware.h"
#include "logic/state_machine.h"
#include "logic/safety.h"
#include "logic/pd_manager.h"
#include "logic/settings.h"
#include "config/version.h"
#include "config/app_config.h"
#include <cstdio>
#include <cstring>

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
    , _last_pdo_selection(-1)
    , _last_adjust_value(0)
    , _last_boot_message(nullptr)
{
}

// ============================================================================
// Initialization
// ============================================================================

void DisplayManager::init() {
    clearScreen();
    _needs_full_redraw = true;
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
        // Draw logo area (centered)
        drawLogo();
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

        drawMenuItem(y, "About", selected == MenuItem::ABOUT);

        _last_menu_selection = sel_idx;
    }

    // Draw hints only on full redraw
    if (_needs_full_redraw) {
        hw.display.drawString(MARGIN, SCREEN_HEIGHT - 30,
                              "Click: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
        hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                              "Long press: Back", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
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
    } else if (mode == AdjustMode::ABOUT) {
        if (_needs_full_redraw) {
            drawHeader("About");
            drawAboutScreen();
        }
    }
}

void DisplayManager::renderFaultScreen() {
    if (_needs_full_redraw) {
        clearScreen();
        drawFaultIcon();
    }

    drawFaultDetails();
}

// ============================================================================
// Common UI Elements
// ============================================================================

void DisplayManager::drawHeader(const char* title) {
    // Clear header area
    hw.display.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, UIColors::BACKGROUND);

    // Draw title centered
    drawCenteredString(10, title, UIColors::TEXT_PRIMARY, 2);

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

void DisplayManager::drawLogo() {
    // Simple text logo for now
    // TODO: Add actual Synapticon logo bitmap
    drawCenteredString(60, "SYNAPTICON", UIColors::SYNAPTICON_PINK, 2);
}

void DisplayManager::drawBootText() {
    // Static text - only draw once on full redraw
    if (_needs_full_redraw) {
        // Product name
        drawCenteredString(110, Version::PRODUCT_NAME, UIColors::TEXT_PRIMARY, 3);

        // Subtitle
        drawCenteredString(150, Version::PRODUCT_SUBTITLE, UIColors::TEXT_SECONDARY, 2);

        // Version
        drawCenteredString(180, Version::FIRMWARE_VERSION, UIColors::MUTED, 1);
    }

    // Stage message - only redraw when message changes (prevents flicker)
    const char* stage_msg = stateMachine.getBootStageMessage();
    if (stage_msg != _last_boot_message) {
        hw.display.fillRect(0, 205, SCREEN_WIDTH, 20, UIColors::BACKGROUND);
        if (stage_msg && stage_msg[0] != '\0') {
            drawCenteredString(210, stage_msg, UIColors::TEXT_SECONDARY, 1);
        }
        _last_boot_message = stage_msg;
    }
}

void DisplayManager::drawBootProgress() {
    uint8_t progress = stateMachine.getBootProgress();
    drawProgressBar(MARGIN * 3, 250, SCREEN_WIDTH - MARGIN * 6, 15, progress, UIColors::SYNAPTICON_PINK);
}

// ============================================================================
// Main Screen Elements
// ============================================================================

void DisplayManager::drawActiveContract() {
    int y = CONTENT_Y_START + 5;

    // Only clear on full redraw
    if (_needs_full_redraw) {
        hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, 45, UIColors::BACKGROUND);
        hw.display.drawString(MARGIN, y, "Contract:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    }

    // Get active contract
    const ActiveContract& contract = pdManager.getActiveContract();

    // Use fixed-width format to avoid clearing
    char line1[32];
    if (contract.valid && contract.voltage_mv > 0) {
        snprintf(line1, sizeof(line1), "%5.1fV @ %5.2fA  ",
                 contract.voltage_mv / 1000.0f,
                 contract.current_ma / 1000.0f);
        hw.display.drawString(MARGIN, y + 15, line1, UIColors::ACCENT, UIColors::BACKGROUND, 2);
    } else {
        // Non-PD charger or no contract: show USB default
        hw.display.drawString(MARGIN, y + 15, "USB 5V (no PD)    ", UIColors::MUTED, UIColors::BACKGROUND, 2);
    }
}

// void DisplayManager::drawPowerReadings() {
//     int y = CONTENT_Y_START + 50;

//     // Get safety state for readings
//     const SafetyState& state = safety.getState();

//     // Use fixed-width format to overwrite previous values without clearing
//     char buf[32];

//     // VBUS Voltage
//     snprintf(buf, sizeof(buf), "VIN: %6.2f V  ", state.vbus_voltage_v);
//     hw.display.drawString(MARGIN, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 1);

//     // INA228 Voltage - use fixed width to avoid clearing
//     y += 15;
//     snprintf(buf, sizeof(buf), "V");
//     hw.display.drawString(MARGIN, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);
//     snprintf(buf, sizeof(buf), "OUT");
//     hw.display.drawString(MARGIN + 12, y+7, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 1);
//     snprintf(buf, sizeof(buf), ": %6.3f V  ", state.ina_voltage_v);
//     hw.display.drawString(MARGIN + 31, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);

//     // Current
//     y += 25;
//     snprintf(buf, sizeof(buf), "I: %6.3f A  ", state.current_a);
//     hw.display.drawString(MARGIN, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);

//     // Power
//     y += 25;
//     snprintf(buf, sizeof(buf), "P: %6.2f W  ", state.power_w);
//     hw.display.drawString(MARGIN, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);
// }

void DisplayManager::drawPowerReadings() {
    // Start slightly lower to give breathing room from the Contract info
    int y = CONTENT_Y_START + 55;
    
    // Layout Constants
    const int LABEL_X = MARGIN;
    const int VALUE_X = MARGIN + 35; // Align all big numbers here
    
    const SafetyState& state = safety.getState();
    char buf[32];

    // --- Voltage Section ---
    // Primary: Output Voltage
    hw.display.drawString(LABEL_X, y + 5, "Vout", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    snprintf(buf, sizeof(buf), "%.3f V", state.ina_voltage_v);
    hw.display.drawString(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);
    
    // Secondary: Input Voltage (Moved below Vout as requested)
    y += 22; 
    snprintf(buf, sizeof(buf), "Vin: %.2f V", state.vbus_voltage_v);
    hw.display.drawString(VALUE_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, 1);

    // --- Current Section ---
    y += 20; // Gap between sections
    
    // Primary: Output Current
    hw.display.drawString(LABEL_X, y + 5, "Iout", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    snprintf(buf, sizeof(buf), "%.3f A", state.current_a);
    hw.display.drawString(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);

    // Secondary: Current Limit (New feature)
    y += 22;
    float limit_a = stateMachine.getCurrentLimitMa() / 1000.0f;
    snprintf(buf, sizeof(buf), "Lim: %.2f A", limit_a);
    // drawn in dark grey (MUTED) as requested
    hw.display.drawString(VALUE_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, 1);

    // --- Power Section ---
    y += 20; // Gap between sections
    
    // Primary: Power
    hw.display.drawString(LABEL_X, y + 5, "Pwr", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    snprintf(buf, sizeof(buf), "%.2f W", state.power_w);
    hw.display.drawString(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);
}

void DisplayManager::drawTemperature() {
    // int y = CONTENT_Y_START + 150;
    int y = CONTENT_Y_START + 170;

    const SafetyState& state = safety.getState();

    // Show both NTC (board) and INA228 (die) temperatures
    char buf[40];
    snprintf(buf, sizeof(buf), "NTC:%5.1fC  INA:%5.1fC",
             state.temperature_c, state.ina_temperature_c);

    uint16_t color = UIColors::TEXT_PRIMARY;
    if (state.temp_status == SafetyStatus::WARNING) {
        color = UIColors::WARNING;
    } else if (state.temp_status == SafetyStatus::FAULT) {
        color = UIColors::ERROR;
    }

    hw.display.drawString(MARGIN, y, buf, color, UIColors::BACKGROUND, 1);
}

void DisplayManager::drawOutputStatus() {
    int y = CONTENT_Y_START + 185;

    // Only draw labels on full redraw
    if (_needs_full_redraw) {
        hw.display.drawString(MARGIN, y, "Load Switch:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
        hw.display.drawString(MARGIN, y + 20, "17V Buck:", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
        hw.display.drawString(MARGIN, y + 45, "Long press: Menu", UIColors::MUTED, UIColors::BACKGROUND, 1);
    }

    // Load switch status - use fixed width "ON " or "OFF"
    bool load_on = hw.loadSwitch.read();
    hw.display.drawString(MARGIN + 90, y, load_on ? "ON " : "OFF",
                          load_on ? UIColors::ACCENT : UIColors::ERROR,
                          UIColors::BACKGROUND, 1);

    // 17V buck status
    bool buck_on = hw.EN_17V.read();
    hw.display.drawString(MARGIN + 90, y + 20, buck_on ? "ON " : "OFF",
                          buck_on ? UIColors::ACCENT : UIColors::MUTED,
                          UIColors::BACKGROUND, 1);
}

// ============================================================================
// Menu Elements
// ============================================================================

void DisplayManager::drawMenuItem(int y, const char* text, bool selected) {
    uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
    uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

    // Single fill with correct background (avoids flicker from clear+highlight)
    hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);

    hw.display.drawString(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, 1);
    hw.display.drawString(MARGIN + 20, y + 5, text, fg, bg, 1);
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
        drawCenteredString(y + 30, "No PD contracts", UIColors::WARNING, 2);
        drawCenteredString(y + 70, "The connected charger may", UIColors::TEXT_SECONDARY, 1);
        drawCenteredString(y + 85, "not support USB Power Delivery.", UIColors::TEXT_SECONDARY, 1);
        drawCenteredString(y + 110, "Try a USB-C PD charger.", UIColors::MUTED, 1);

        if (_needs_full_redraw) {
            hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                                  "Long press: Back", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
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

        hw.display.drawString(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, 1);
        hw.display.drawString(MARGIN + 20, y + 5, line, fg, bg, 1);

        y += MENU_ITEM_HEIGHT;
    }

    // Clear remaining slots if less than 8 visible
    for (int i = visible_count; i < 8; i++) {
        hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, UIColors::BACKGROUND);
        y += MENU_ITEM_HEIGHT;
    }

    // Draw hints only on full redraw
    if (_needs_full_redraw) {
        hw.display.drawString(MARGIN, SCREEN_HEIGHT - 30,
                              "Click: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
        hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                              "Long press: Cancel", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
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

    // Draw current value - fixed width format
    char buf[32];
    snprintf(buf, sizeof(buf), "%5.2f A", current_ma / 1000.0f);
    drawCenteredString(y, buf, UIColors::ACCENT, 3);

    // Draw progress bar (scaled to effective max)
    y += 60;
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

        hw.display.drawString(MARGIN * 2, y, min_str, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);

        // Right-align max
        int max_width = strlen(max_str) * 6;
        hw.display.drawString(SCREEN_WIDTH - MARGIN * 2 - max_width, y, max_str,
                              UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);

        hw.display.drawString(MARGIN, SCREEN_HEIGHT - 30,
                              "Rotate: Adjust", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
        hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                              "Click: Confirm  Long: Cancel", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    }
}

// ============================================================================
// About Screen
// ============================================================================

void DisplayManager::drawAboutScreen() {
    int y = CONTENT_Y_START + 15;
    const int LINE_H = 18;

    // Product name (large)
    drawCenteredString(y, Version::PRODUCT_NAME, UIColors::SYNAPTICON_PINK, 3);
    y += 35;

    // Subtitle
    drawCenteredString(y, Version::PRODUCT_SUBTITLE, UIColors::TEXT_PRIMARY, 2);
    y += 30;

    // Separator
    hw.display.drawLine(MARGIN * 3, y, SCREEN_WIDTH - MARGIN * 3, y, UIColors::HEADER_LINE);
    y += 12;

    // Info lines
    char buf[40];

    snprintf(buf, sizeof(buf), "Version: %s", Version::FIRMWARE_VERSION);
    hw.display.drawString(MARGIN * 2, y, buf, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    y += LINE_H;

    snprintf(buf, sizeof(buf), "Author:  %s", Version::AUTHOR);
    hw.display.drawString(MARGIN * 2, y, buf, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    y += LINE_H;

    snprintf(buf, sizeof(buf), "Built:   %s %s", BUILD_DATE, BUILD_TIME);
    hw.display.drawString(MARGIN * 2, y, buf, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    y += LINE_H;

    hw.display.drawString(MARGIN * 2, y, "Target:  RP2040 (Pico)", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    y += LINE_H;

    hw.display.drawString(MARGIN * 2, y, "Max:     48V 5A (240W)", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    y += LINE_H + 8;

    // Separator
    hw.display.drawLine(MARGIN * 3, y, SCREEN_WIDTH - MARGIN * 3, y, UIColors::HEADER_LINE);
    y += 12;

    drawCenteredString(y, "SYNAPTICON GmbH", UIColors::SYNAPTICON_PINK, 1);

    // Navigation hint
    hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                          "Click or Long press: Back", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
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

    // Draw "!" symbol
    hw.display.drawString(cx - 10, cy - 20, "!", UIColors::ERROR, UIColors::BACKGROUND, 4);

    // Draw FAULT header
    drawCenteredString(cy + 30, "FAULT", UIColors::ERROR, 3);
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
            snprintf(detail1, sizeof(detail1), "Measured: %.1fC", safety.getState().temperature_c);
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

    drawCenteredString(y, fault_name, UIColors::ERROR, 2);

    y += 40;
    if (detail1[0]) {
        drawCenteredString(y, detail1, UIColors::TEXT_PRIMARY, 1);
        y += 20;
    }
    if (detail2[0]) {
        drawCenteredString(y, detail2, UIColors::TEXT_PRIMARY, 1);
        y += 20;
    }

    y += 20;
    drawCenteredString(y, "Load switch disabled", UIColors::WARNING, 1);

    // Draw acknowledge hint
    drawCenteredString(SCREEN_HEIGHT - 30, "[Click to acknowledge]", UIColors::TEXT_SECONDARY, 1);
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
