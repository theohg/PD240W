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
    drawStatusBar();
}

void DisplayManager::renderMenuScreen() {
    if (_needs_full_redraw) {
        drawHeader("Menu");
    }

    MenuItem selected = stateMachine.getSelectedMenuItem();

    int y = CONTENT_Y_START + 10;

    drawMenuItem(y, "Select Voltage", selected == MenuItem::SELECT_VOLTAGE);
    y += MENU_ITEM_HEIGHT;

    drawMenuItem(y, "Current Limit", selected == MenuItem::CURRENT_LIMIT);
    y += MENU_ITEM_HEIGHT;

    drawMenuItem(y, "About", selected == MenuItem::ABOUT);

    // Draw hint at bottom
    hw.display.drawString(MARGIN, SCREEN_HEIGHT - 30,
                          "Click: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                          "Long press: Back", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
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

void DisplayManager::drawStatusBar() {
    int y = SCREEN_HEIGHT - STATUS_BAR_HEIGHT;

    // Draw output status indicators - fixed width format avoids clearing
    bool load_on = hw.loadSwitch.read();
    bool buck_on = hw.EN_17V.read();

    char status[32];
    snprintf(status, sizeof(status), "OUT:%s  17V:%s  ",
             load_on ? "ON " : "OFF",
             buck_on ? "ON " : "OFF");

    hw.display.drawString(MARGIN, y + 3, status,
                          load_on ? UIColors::ACCENT : UIColors::MUTED,
                          UIColors::BACKGROUND, 1);
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
    drawCenteredString(60, "SYNAPTICON", UIColors::ACCENT, 2);
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

    // Stage message - only clear and redraw the message area (small)
    hw.display.fillRect(0, 205, SCREEN_WIDTH, 20, UIColors::BACKGROUND);
    const char* stage_msg = stateMachine.getBootStageMessage();
    if (stage_msg && stage_msg[0] != '\0') {
        drawCenteredString(210, stage_msg, UIColors::TEXT_SECONDARY, 1);
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
    if (contract.valid) {
        snprintf(line1, sizeof(line1), "%5.1fV @ %5.1fA  ",
                 contract.voltage_mv / 1000.0f,
                 contract.current_ma / 1000.0f);
        hw.display.drawString(MARGIN, y + 15, line1, UIColors::ACCENT, UIColors::BACKGROUND, 2);
    } else {
        hw.display.drawString(MARGIN, y + 15, "No contract       ", UIColors::WARNING, UIColors::BACKGROUND, 2);
    }
}

void DisplayManager::drawPowerReadings() {
    int y = CONTENT_Y_START + 60;

    // Get safety state for readings
    const SafetyState& state = safety.getState();

    // Use fixed-width format to overwrite previous values without clearing
    char buf[32];

    // Voltage - use fixed width to avoid clearing
    snprintf(buf, sizeof(buf), "V: %6.2f V  ", state.vbus_voltage_v);
    hw.display.drawString(MARGIN, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);

    // Current
    y += 25;
    snprintf(buf, sizeof(buf), "I: %6.3f A  ", state.current_a);
    hw.display.drawString(MARGIN, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);

    // Power
    y += 25;
    snprintf(buf, sizeof(buf), "P: %6.2f W  ", state.power_w);
    hw.display.drawString(MARGIN, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, 2);
}

void DisplayManager::drawTemperature() {
    int y = CONTENT_Y_START + 150;

    const SafetyState& state = safety.getState();

    // Use fixed-width format
    char buf[32];
    snprintf(buf, sizeof(buf), "Temp: %5.1f C  ", state.temperature_c);

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
    // Clear line
    hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT, UIColors::BACKGROUND);

    uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
    uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

    if (selected) {
        hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);
        hw.display.drawString(MARGIN + 5, y + 5, ">", fg, bg, 1);
    }

    hw.display.drawString(MARGIN + 20, y + 5, text, fg, bg, 1);
}

void DisplayManager::drawPdoList() {
    int y = CONTENT_Y_START + 10;
    int8_t selected_idx = stateMachine.getSelectedPdoIndex();

    // Get PDO list from PD manager
    SourceCapability pdos[13];
    uint8_t count = pdManager.getSourceCapabilities(pdos, 13);

    // Clear content area
    hw.display.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START - 40, UIColors::BACKGROUND);

    // Draw visible PDOs (max 8 visible at once)
    int start_idx = 0;
    if (selected_idx > 5 && count > 8) {
        start_idx = selected_idx - 5;
        if (start_idx + 8 > count) {
            start_idx = count - 8;
        }
    }

    for (int i = start_idx; i < count && i < start_idx + 8; i++) {
        bool selected = (i == selected_idx);

        uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
        uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

        if (selected) {
            hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);
        }

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

        if (selected) {
            hw.display.drawString(MARGIN + 5, y + 5, ">", fg, bg, 1);
        }
        hw.display.drawString(MARGIN + 20, y + 5, line, fg, bg, 1);

        y += MENU_ITEM_HEIGHT;
    }

    // Draw hints
    hw.display.drawString(MARGIN, SCREEN_HEIGHT - 30,
                          "Click: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                          "Long press: Cancel", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
}

void DisplayManager::drawCurrentLimitAdjust() {
    int y = CONTENT_Y_START + 40;

    // Clear content area
    hw.display.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START - 40, UIColors::BACKGROUND);

    uint32_t current_ma = stateMachine.getCurrentLimitMa();

    // Draw current value
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f A", current_ma / 1000.0f);
    drawCenteredString(y, buf, UIColors::ACCENT, 3);

    // Draw progress bar
    y += 60;
    uint8_t percent = ((current_ma - AppConfig::CURRENT_LIMIT_MIN_MA) * 100) /
                      (AppConfig::CURRENT_LIMIT_MAX_MA - AppConfig::CURRENT_LIMIT_MIN_MA);
    drawProgressBar(MARGIN * 2, y, SCREEN_WIDTH - MARGIN * 4, 20, percent, UIColors::ACCENT);

    // Draw min/max labels
    y += 30;
    char min_str[16], max_str[16];
    snprintf(min_str, sizeof(min_str), "%.1fA", AppConfig::CURRENT_LIMIT_MIN_MA / 1000.0f);
    snprintf(max_str, sizeof(max_str), "%.1fA", AppConfig::CURRENT_LIMIT_MAX_MA / 1000.0f);

    hw.display.drawString(MARGIN * 2, y, min_str, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);

    // Right-align max
    int max_width = strlen(max_str) * 6;  // Approximate width
    hw.display.drawString(SCREEN_WIDTH - MARGIN * 2 - max_width, y, max_str,
                          UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);

    // Draw hints
    hw.display.drawString(MARGIN, SCREEN_HEIGHT - 30,
                          "Rotate: Adjust", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
    hw.display.drawString(MARGIN, SCREEN_HEIGHT - 15,
                          "Click: Confirm  Long: Cancel", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, 1);
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
