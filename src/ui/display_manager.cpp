#include "display_manager.h"
#include "hardware.h"
#include "logic/state_machine.h"
#include "logic/tps_eeprom_workflow.h"
#include "logic/safety.h"
#include "logic/pd_manager.h"
#include "logic/settings.h"
#include "logic/cc_controller.h"
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

// Overtemperature fault screen layout (vertically aligned columns)
static constexpr int OT_LABEL_X = 45;   // Labels: "Trigger", "Limit", "Now"
static constexpr int OT_COLON_X = 118;   // ":" column (vertically aligned)
static constexpr int OT_VALUE_X = 130;   // Temperature values (left-aligned digits)
static constexpr int OT_UNIT_X  = 180;  // "°C" column (vertically aligned)
static constexpr int OT_ROW_H   = 22;   // Row spacing

// Overcurrent fault screen layout (vertically aligned columns)
static constexpr int OC_LABEL_X = 45;   // Labels: "Trigger", "Limit", "Now"
static constexpr int OC_COLON_X = 118;   // ":" column (vertically aligned)
static constexpr int OC_VALUE_X = 130;  // Current values (left-aligned digits)
static constexpr int OC_UNIT_X  = 180;  // "A" column (vertically aligned)
static constexpr int OC_ROW_H   = 22;   // Row spacing

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
    , _last_pdo_scroll_idx(-1)
    , _last_adjust_value(0)
    , _last_pps_voltage(0)
    , _last_avs_voltage(0)
    , _last_pps_state(-1)
    , _last_pd_revision_drawn(false)
    , _last_pd_revision{0}
    , _last_epr_badge_drawn(false)
    , _last_brightness_value(255)
    , _last_boot_message(nullptr)
    , _backlight_on(false)
    , _last_auto_pps(false)
    , _last_auto_avs(false)
    , _last_auto_output(false)
    , _last_sounds(true)
    , _last_dim_timeout(1)
    , _last_melody(1)
    , _last_brightness_adjusting(false)
    , _last_dim_adjusting(false)
    , _last_melody_adjusting(false)
    , _last_pps_converged(false)
    , _last_avs_converged(false)
    , _last_cc_badge_state(-1)
    , _last_cc_adjust_state(-1)
    , _last_energy_mode(-1)
    , _fault_now_temp_y(219)
{
}

// ============================================================================
// Initialization
// ============================================================================

void DisplayManager::init() {
    clearScreen();
    _needs_full_redraw = true;
    _last_pps_state = -1;  // Force PPS/AVS badge redraw on first render
    _last_pd_revision_drawn = false;  // Force PD revision badge redraw
    _last_pd_revision[0] = '\0';
    _last_epr_badge_drawn = false;  // Force EPR badge redraw
    _last_cc_badge_state = -1;     // Force CC badge redraw
    _last_cc_adjust_state = -1;    // Force CC adjust badge redraw
    _last_energy_mode = -1;        // Force energy unit redraw
    _last_pdo_scroll_idx = -1;  // Reset scroll position
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
    _last_pdo_scroll_idx = -1;  // Reset scroll position on invalidate
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

    // Only redraw the previously-selected and newly-selected items
    bool sel_changed = (sel_idx != _last_menu_selection);
    auto sel_affects = [&](int idx) {
        return sel_changed && (idx == sel_idx || idx == _last_menu_selection);
    };

    int y = CONTENT_Y_START + 10;

    if (_needs_full_redraw || sel_affects(0))
        drawMenuItem(y, "Select Voltage", selected == MenuItem::SELECT_VOLTAGE);
    y += MENU_ITEM_HEIGHT;

    if (_needs_full_redraw || sel_affects(1))
        drawMenuItem(y, "Current Limit", selected == MenuItem::CURRENT_LIMIT);
    y += MENU_ITEM_HEIGHT;

    if (_needs_full_redraw || sel_affects(2))
        drawMenuItem(y, "Settings", selected == MenuItem::SETTINGS);
    y += MENU_ITEM_HEIGHT;

    if (_needs_full_redraw || sel_affects(3))
        drawMenuItem(y, "About", selected == MenuItem::ABOUT);
    y += MENU_ITEM_HEIGHT;

    if (_needs_full_redraw || sel_affects(4))
        drawMenuItemMuted(y, "Back", selected == MenuItem::BACK);

    _last_menu_selection = sel_idx;

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
    } else if (mode == AdjustMode::AVS_VOLTAGE) {
        if (_needs_full_redraw) {
            drawHeader("AVS Voltage");
        }
        drawAvsVoltageAdjust();
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
        // Clear full width with font lineHeight to remove AA artifacts from previous text
        hw.display.fillRect(0, 255, SCREEN_WIDTH, FONT_SMALL->lineHeight + 2, UIColors::BACKGROUND);
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
        _last_epr_badge_drawn = false;  // Force redraw of EPR badge
    }

    // Get active contract
    const ActiveContract& contract = pdManager.getActiveContract();

    // Badge constants
    const int BADGE_H = 18;
    const int BADGE_R = 4;

    // Use fixed-width format to avoid clearing
    char line1[32];
    if (contract.valid && contract.voltage_mv > 0) {
        // Always show the actual negotiated contract from the charger
        snprintf(line1, sizeof(line1), "%5.2fV @ %5.2fA  ",
                 contract.voltage_mv / 1000.0f,
                 contract.current_ma / 1000.0f);
        hw.display.drawStringAA(MARGIN, y + 16, line1, UIColors::ACCENT, UIColors::BACKGROUND, FONT_MEDIUM);

        // PD revision badge (draw when revision changes or on full redraw)
        const char* pd_rev = pdManager.getPdRevision();
        bool rev_changed = (strcmp(pd_rev, _last_pd_revision) != 0);

        // Rightmost badge position (PPS/AVS badge)
        const int RIGHTMOST_BADGE_X = SCREEN_WIDTH - MARGIN - 36;
        const int RIGHTMOST_BADGE_W = 40;

        if (pd_rev[0] != '\0' && (_needs_full_redraw || !_last_pd_revision_drawn || rev_changed)) {
            // Clear old badge area if revision string changed (different width)
            if (rev_changed && _last_pd_revision_drawn) {
                int old_w = ST7789::getStringWidthAA(_last_pd_revision, FONT_SMALL) + 8;
                int old_x = RIGHTMOST_BADGE_X - old_w - 4;
                hw.display.fillRect(old_x, y - 2, old_w, BADGE_H, UIColors::BACKGROUND);
            }
            int rev_w = ST7789::getStringWidthAA(pd_rev, FONT_SMALL) + 8;
            int rev_x = RIGHTMOST_BADGE_X - rev_w - 4;
            int rev_y = y - 2;
            hw.display.fillRoundRect(rev_x, rev_y, rev_w, BADGE_H, BADGE_R, UIColors::MUTED);
            int text_x = rev_x + (rev_w - ST7789::getStringWidthAA(pd_rev, FONT_SMALL)) / 2;
            int text_y = rev_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(text_x, text_y, pd_rev, UIColors::TEXT_PRIMARY, UIColors::MUTED, FONT_SMALL);
            _last_pd_revision_drawn = true;
            strncpy(_last_pd_revision, pd_rev, sizeof(_last_pd_revision) - 1);
            _last_pd_revision[sizeof(_last_pd_revision) - 1] = '\0';
        }

        // EPR badge: show when active contract is AVS or voltage > 20V (EPR territory)
        // Evaluated independently of PD revision badge to update when contract changes
        if (_last_pd_revision_drawn) {
            int rev_w = ST7789::getStringWidthAA(_last_pd_revision, FONT_SMALL) + 8;
            int rev_x = RIGHTMOST_BADGE_X - rev_w - 4;
            bool show_epr = contract.is_avs || (contract.valid && contract.voltage_mv > 20000);
            if (show_epr && !_last_epr_badge_drawn) {
                const char* epr_text = "EPR";
                int epr_w = ST7789::getStringWidthAA(epr_text, FONT_SMALL) + 8;
                int epr_x = rev_x - epr_w - 4;
                int epr_y = y - 2;
                hw.display.fillRoundRect(epr_x, epr_y, epr_w, BADGE_H, BADGE_R, UIColors::SYNAPTICON_PINK);
                int epr_tx = epr_x + (epr_w - ST7789::getStringWidthAA(epr_text, FONT_SMALL)) / 2;
                int epr_ty = epr_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
                hw.display.drawStringAA(epr_tx, epr_ty, epr_text, UIColors::TEXT_PRIMARY, UIColors::SYNAPTICON_PINK, FONT_SMALL);
                _last_epr_badge_drawn = true;
            } else if (!show_epr && _last_epr_badge_drawn) {
                // Clear EPR badge
                int epr_w = ST7789::getStringWidthAA("EPR", FONT_SMALL) + 8;
                int epr_x = rev_x - epr_w - 4;
                hw.display.fillRect(epr_x, y - 3, epr_w + 4, BADGE_H + 2, UIColors::BACKGROUND);
                _last_epr_badge_drawn = false;
            }
        }

        // Show PPS/AVS indicator if active - only redraw when state changes
        if (contract.is_pps) {
            bool tuning_converged = pdManager.isPpsTuningConverged();
            bool tuning_active = pdManager.isPpsTuningActive();

            // Redraw badge when PPS state changes OR convergence state changes
            if (_last_pps_state != 1 || (tuning_active && tuning_converged != _last_pps_converged)) {
                int badge_x = RIGHTMOST_BADGE_X;
                int badge_y = y - 2;
                int badge_w = RIGHTMOST_BADGE_W;

                // Yellow badge while tuning is converging, green when converged or auto-tune off
                uint16_t badge_color = (tuning_active && !tuning_converged)
                    ? UIColors::CAUTION : UIColors::ACCENT;

                hw.display.fillRoundRect(badge_x, badge_y, badge_w, BADGE_H, BADGE_R, badge_color);
                int text_x = badge_x + (badge_w - ST7789::getStringWidthAA("PPS", FONT_SMALL)) / 2;
                int text_y = badge_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
                hw.display.drawStringAA(text_x, text_y, "PPS", UIColors::BACKGROUND, badge_color, FONT_SMALL);
                _last_pps_state = 1;
                _last_pps_converged = tuning_converged;
            }
        } else if (contract.is_avs) {
            // AVS badge - with tuning convergence colors (like PPS), same position
            bool tuning_converged = pdManager.isAvsTuningConverged();
            bool tuning_active = pdManager.isAvsTuningActive();

            // Redraw badge when AVS state changes OR convergence state changes
            if (_last_pps_state != 2 || (tuning_active && tuning_converged != _last_avs_converged)) {
                int badge_x = RIGHTMOST_BADGE_X;
                int badge_y = y - 2;
                int badge_w = RIGHTMOST_BADGE_W;

                // Yellow badge while tuning is converging, green when converged or auto-tune off
                uint16_t badge_color = (tuning_active && !tuning_converged)
                    ? UIColors::CAUTION : UIColors::ACCENT;

                hw.display.fillRoundRect(badge_x, badge_y, badge_w, BADGE_H, BADGE_R, badge_color);
                int text_x = badge_x + (badge_w - ST7789::getStringWidthAA("AVS", FONT_SMALL)) / 2;
                int text_y = badge_y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
                hw.display.drawStringAA(text_x, text_y, "AVS", UIColors::BACKGROUND, badge_color, FONT_SMALL);
                _last_pps_state = 2;
                _last_avs_converged = tuning_converged;
            }
        } else {
            // Clear badge area when neither PPS nor AVS active
            if (_last_pps_state != 0) {
                hw.display.fillRect(RIGHTMOST_BADGE_X - 2, y - 3, RIGHTMOST_BADGE_W + 4, 22, UIColors::BACKGROUND);
                _last_pps_state = 0;
            }
        }
    } else {
        // Non-PD charger or no contract: show USB default
        hw.display.drawStringAA(MARGIN, y + 16, "USB 5V (no PD)    ", UIColors::MUTED, UIColors::BACKGROUND, FONT_MEDIUM);
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
    const int FRAME_H = 152;  // Includes energy line
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
    const int UNIT_X  = 155;         // Fixed X for unit letters (V, A, W) — prevents shifting

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

    // Secondary line: Vin (pre-switch VBUS) always on left, Vset (user target) on right when PPS/AVS
    y += 32;
    {
        // Vin always on left at the same position
        const int VIN_VALUE_X = VALUE_X + 30;
        const int VIN_UNIT_X = VALUE_X + 72;
        hw.display.drawStringAA(VALUE_X, y, "Vin:", UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
        snprintf(buf, sizeof(buf), "%5.2f", state.vbus_voltage_v);
        hw.display.drawStringAA(VIN_VALUE_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
        int vin_w = ST7789::getStringWidthAA(buf, FONT_SMALL);
        if (VIN_VALUE_X + vin_w < VIN_UNIT_X)
            hw.display.fillRect(VIN_VALUE_X + vin_w, y, VIN_UNIT_X - VIN_VALUE_X - vin_w, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
        hw.display.drawStringAA(VIN_UNIT_X, y, "V", UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);

        // Vset on right when PPS or AVS target is known
        uint32_t vset_mv = 0;
        if (pdManager.isPpsActive() && pdManager.getPpsUserTargetMv() > 0) {
            vset_mv = pdManager.getPpsUserTargetMv();
        } else if (pdManager.isAvsActive() && pdManager.getAvsUserTargetMv() > 0) {
            vset_mv = pdManager.getAvsUserTargetMv();
        }

        const int VSET_LABEL_X = VIN_UNIT_X + 16;
        const int VSET_VAL_X = VSET_LABEL_X + 36;
        const int VSET_UNIT_X = VSET_VAL_X + 42;
        if (vset_mv > 0) {
            hw.display.drawStringAA(VSET_LABEL_X, y, "Vset:", UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
            snprintf(buf, sizeof(buf), "%5.2f", vset_mv / 1000.0f);
            hw.display.drawStringAA(VSET_VAL_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
            int vset_w = ST7789::getStringWidthAA(buf, FONT_SMALL);
            if (VSET_VAL_X + vset_w < VSET_UNIT_X)
                hw.display.fillRect(VSET_VAL_X + vset_w, y, VSET_UNIT_X - VSET_VAL_X - vset_w, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
            hw.display.drawStringAA(VSET_UNIT_X, y, "V", UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
        } else {
            // Clear Vset area when not in PPS/AVS
            hw.display.fillRect(VSET_LABEL_X, y, VSET_UNIT_X + 10 - VSET_LABEL_X, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
        }
    }

    // --- Current Section ---
    y += 14; // Gap between sections

    // Clamp small negative current to zero (sink-only device, noise below 5mA)
    float display_current = state.current_a;
    if (display_current > -0.005f && display_current < 0.0005f) {
        display_current = 0.0f;
    }

    // Primary: Output Current (number and unit rendered separately)
    hw.display.drawStringAA(LABEL_X, y + 8, "Iout", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    snprintf(buf, sizeof(buf), "%.3f", display_current);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);
    num_w = ST7789::getStringWidthAA(buf, FONT_LARGE);
    if (VALUE_X + num_w < UNIT_X)
        hw.display.fillRect(VALUE_X + num_w, y, UNIT_X - VALUE_X - num_w, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(UNIT_X, y, "A", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);

    // OCP/CC mode badge (next to "A" on current row)
    // OCP always shown when not in PPS/AVS (regardless of CC setting)
    // CC shown only in PPS/AVS: grey=not regulating, white=regulating
    {
        bool cc_enabled = CcController::isEnabled();
        bool in_pps_avs = pdManager.isPpsActive() || pdManager.isAvsActive();
        bool cc_regulating = CcController::isRegulating();
        // 0=OCP grey, 1=CC white (regulating), 2=CC grey (enabled+PPS/AVS, not regulating)
        int8_t badge_state;
        if (cc_enabled && in_pps_avs) {
            badge_state = cc_regulating ? 1 : 2;
        } else {
            badge_state = 0;  // OCP
        }
        if (badge_state != _last_cc_badge_state || _needs_full_redraw) {
            const int BADGE_AREA_X = UNIT_X + ST7789::getStringWidthAA("A", FONT_LARGE) + 6;
            const int BADGE_H = 16;
            const int BADGE_Y = y + (FONT_LARGE->lineHeight - BADGE_H) / 2;
            // OCP is the widest badge — use its width as the clear/centering area
            int ocp_w = ST7789::getStringWidthAA("OCP", FONT_SMALL) + 8;
            hw.display.fillRect(BADGE_AREA_X, BADGE_Y, ocp_w, BADGE_H, UIColors::BACKGROUND);

            const char* txt;
            uint16_t badge_color;
            if (badge_state == 1) {
                txt = "CC"; badge_color = UIColors::TEXT_PRIMARY;
            } else if (badge_state == 2) {
                txt = "CC"; badge_color = UIColors::MUTED;
            } else {
                txt = "OCP"; badge_color = UIColors::MUTED;
            }
            int badge_w = ST7789::getStringWidthAA(txt, FONT_SMALL) + 8;
            int badge_x = BADGE_AREA_X + (ocp_w - badge_w) / 2;
            hw.display.fillRoundRect(badge_x, BADGE_Y, badge_w, BADGE_H, 3, badge_color);
            int tx = badge_x + (badge_w - ST7789::getStringWidthAA(txt, FONT_SMALL)) / 2;
            int ty = BADGE_Y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
            hw.display.drawStringAA(tx, ty, txt, UIColors::BACKGROUND, badge_color, FONT_SMALL);
            _last_cc_badge_state = badge_state;
        }
    }

    // Secondary: Current Limit
    y += 32;
    float limit_a = stateMachine.getCurrentLimitMa() / 1000.0f;
    snprintf(buf, sizeof(buf), "Lim: %.2f A", limit_a);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);

    // --- Power Section ---
    y += 14; // Gap between sections

    // Primary: Power (number and unit rendered separately)
    // Zero out power when current displays as zero (consistent with current reading)
    float display_power = (display_current == 0.0f) ? 0.0f : state.power_w;
    hw.display.drawStringAA(LABEL_X, y + 8, "Pwr", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    snprintf(buf, sizeof(buf), "%.3f", display_power);
    hw.display.drawStringAA(VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);
    num_w = ST7789::getStringWidthAA(buf, FONT_LARGE);
    if (VALUE_X + num_w < UNIT_X)
        hw.display.fillRect(VALUE_X + num_w, y, UNIT_X - VALUE_X - num_w, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(UNIT_X, y, "W", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_LARGE);

    // --- Energy Section (mAh or mWh since boot, toggled by long press) ---
    y += 32;
    {
        bool show_mwh = stateMachine.isEnergyDisplayMwh();
        double value;
        if (show_mwh) {
            double energy_j = hw.powerMonitor.getEnergy();
            value = energy_j * (1e3 / 3600.0);
        } else {
            double charge_c = hw.powerMonitor.getCharge();
            value = charge_c * 1000.0 / 3.6;
        }
        if (value < 0.0) value = 0.0;

        // Track previous value and unit to avoid flicker (only redraw on change)
        static double last_value = -1.0;
        static bool last_was_high = false;
        // mAh mode: high = Ah (>=1000 mAh), mWh mode: high = Wh (>=1000 mWh)
        bool is_high = (value >= 1000.0);

        // Check if energy display mode changed
        int8_t current_mode = show_mwh ? 1 : 0;
        bool mode_changed = (current_mode != _last_energy_mode);
        if (mode_changed) _last_energy_mode = current_mode;

        // Quantize to display resolution to reduce unnecessary redraws
        bool value_changed = _needs_full_redraw || mode_changed;
        if (is_high) {
            int32_t quantized = (int32_t)(value / 10.0);
            int32_t last_quantized = (int32_t)(last_value / 10.0);
            if (quantized != last_quantized) value_changed = true;
        } else {
            int32_t quantized = (int32_t)(value * 10.0);
            int32_t last_quantized = (int32_t)(last_value * 10.0);
            if (quantized != last_quantized) value_changed = true;
        }
        if (is_high != last_was_high) value_changed = true;

        if (value_changed) {
            const int NRG_VALUE_X = VALUE_X + 30;
            const int NRG_UNIT_X = NRG_VALUE_X + 48;  // Fixed unit position

            // Draw label only on full redraw or mode change
            if (_needs_full_redraw || mode_changed) {
                hw.display.drawStringAA(VALUE_X, y, "Nrg:", UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
            }

            // Overwrite value directly (fixed-width format covers previous digits)
            if (is_high) {
                snprintf(buf, sizeof(buf), "%6.2f", value / 1000.0);
            } else {
                snprintf(buf, sizeof(buf), "%6.1f", value);
            }
            hw.display.drawStringAA(NRG_VALUE_X, y, buf, UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);

            // Gap-fill between value and unit position
            int nrg_w = ST7789::getStringWidthAA(buf, FONT_SMALL);
            if (NRG_VALUE_X + nrg_w < NRG_UNIT_X)
                hw.display.fillRect(NRG_VALUE_X + nrg_w, y, NRG_UNIT_X - NRG_VALUE_X - nrg_w, FONT_SMALL->lineHeight, UIColors::BACKGROUND);

            // Redraw unit text when it changes or on full redraw/mode change
            if (is_high != last_was_high || _needs_full_redraw || mode_changed) {
                hw.display.fillRect(NRG_UNIT_X, y, SCREEN_WIDTH - MARGIN - NRG_UNIT_X, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
                const char* unit;
                if (show_mwh) {
                    unit = is_high ? "Wh" : "mWh";
                } else {
                    unit = is_high ? "Ah" : "mAh";
                }
                hw.display.drawStringAA(NRG_UNIT_X, y, unit, UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
            }

            last_value = value;
            last_was_high = is_high;
        }
    }
}

void DisplayManager::drawTemperature() {
    // Position below the power readings frame (frame ends at CONTENT_Y_START + 45 + 152 = 197)
    int y = CONTENT_Y_START + 202;
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
        // Blink fast & make a beeping sound
        if ((ms / AppConfig::CRITICAL_BLINK_INTERVAL_MS) % 2 == 1) {
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
    const int VALUE_WIDTH = 48;  // Width for numeric value (wide enough to clear AA text artifacts)

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
            // Gap-fill between value end and unit position (prevents artifacts with proportional font)
            int num_w = ST7789::getStringWidthAA(buf, FONT_SMALL);
            if (NTC_VALUE_X + num_w < NTC_UNIT_X)
                hw.display.fillRect(NTC_VALUE_X + num_w, y, NTC_UNIT_X - NTC_VALUE_X - num_w, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
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
            // Gap-fill between value end and unit position (prevents artifacts with proportional font)
            int num_w = ST7789::getStringWidthAA(buf, FONT_SMALL);
            if (INA_VALUE_X + num_w < INA_UNIT_X)
                hw.display.fillRect(INA_VALUE_X + num_w, y, INA_UNIT_X - INA_VALUE_X - num_w, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
        }
        last_ina_temp = state.ina_temperature_c;
    }

    last_blink_hide = blink_hide;
}

void DisplayManager::drawOutputStatus() {
    // Position below temperature readings
    int y = CONTENT_Y_START + 222;

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
        // hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20, "Click: Menu", UIColors::MUTED, UIColors::BACKGROUND, FONT_SMALL);
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

    // Get PDO list and active contract for highlighting
    SourceCapability pdos[AppConfig::MAX_PDO_COUNT];
    uint8_t count = pdManager.getSourceCapabilities(pdos, AppConfig::MAX_PDO_COUNT);
    const ActiveContract& active = pdManager.getActiveContract();

    // No contracts found - show informational message
    if (count == 0) {
        int y = CONTENT_Y_START + 10;
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

    // Total items: PDOs + Back
    int total_items = count + 1;  // +1 for "Back" entry

    // Calculate scroll position
    int start_idx = 0;
    if (selected_idx > 5 && total_items > 8) {
        start_idx = selected_idx - 5;
        if (start_idx + 8 > total_items) {
            start_idx = total_items - 8;
        }
    }

    // Track scroll position to detect when all items need redrawing
    bool scroll_changed = (start_idx != _last_pdo_scroll_idx);
    bool sel_changed = (selected_idx != _last_pdo_selection);

    // Skip redraw if nothing changed
    if (!_needs_full_redraw && !sel_changed && !scroll_changed) {
        return;
    }

    int visible_count = (total_items - start_idx > 8) ? 8 : (total_items - start_idx);
    int y = CONTENT_Y_START + 10;

    for (int i = start_idx; i < start_idx + visible_count; i++) {
        // Only redraw items that need it: full redraw, scroll changed, or selection affects this item
        bool needs_draw = _needs_full_redraw || scroll_changed;
        if (!needs_draw && sel_changed) {
            needs_draw = (i == selected_idx || i == _last_pdo_selection);
        }

        if (needs_draw) {
            bool selected = (i == selected_idx);

            // "Back" item at index == count
            if (i == count) {
                drawMenuItemMuted(y, "Back", selected);
            } else {
                // Check if this PDO is the currently active (negotiated) contract
                bool is_active = false;
                if (active.valid) {
                    if (pdos[i].is_pps && active.is_pps) {
                        is_active = (active.voltage_mv >= pdos[i].min_voltage_mv &&
                                     active.voltage_mv <= pdos[i].voltage_mv &&
                                     active.current_ma == pdos[i].max_current_ma);
                    } else if (pdos[i].is_avs && active.is_avs) {
                        is_active = (active.voltage_mv >= pdos[i].min_voltage_mv &&
                                     active.voltage_mv <= pdos[i].voltage_mv);
                    } else if (!pdos[i].is_pps && !pdos[i].is_avs && !active.is_pps && !active.is_avs) {
                        is_active = (pdos[i].voltage_mv == active.voltage_mv);
                    }
                }

                uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
                uint16_t fg;
                if (is_active) {
                    fg = UIColors::ACCENT;  // Green for active contract
                } else if (selected) {
                    fg = UIColors::HIGHLIGHT_FG;  // Yellow for cursor
                } else {
                    fg = UIColors::TEXT_PRIMARY;  // White for normal
                }

                // Prefix: * for active contract, > for cursor, space for normal
                const char* prefix;
                if (is_active) {
                    prefix = "*";
                } else if (selected) {
                    prefix = ">";
                } else {
                    prefix = " ";
                }

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

                hw.display.drawStringAA(MARGIN + 5, y + 5, prefix, fg, bg, FONT_SMALL);
                hw.display.drawStringAA(MARGIN + 20, y + 5, line, fg, bg, FONT_SMALL);
            }
        }

        y += MENU_ITEM_HEIGHT;
    }

    // Clear remaining slots only on full redraw or scroll change
    if (_needs_full_redraw || scroll_changed) {
        for (int i = visible_count; i < 8; i++) {
            hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, UIColors::BACKGROUND);
            y += MENU_ITEM_HEIGHT;
        }
    }

    _last_pdo_scroll_idx = start_idx;
    _last_pdo_selection = selected_idx;

    // Draw hint only on full redraw
    if (_needs_full_redraw) {
        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 20,
                              "Click: Select", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
    }
}

void DisplayManager::drawCurrentLimitAdjust() {
    uint32_t current_ma = stateMachine.getCurrentLimitMa();
    uint32_t max_ma = stateMachine.getEffectiveMaxCurrentMa();
    bool cc_enabled = CcController::isEnabled();
    int8_t cc_state = cc_enabled ? 1 : 0;
    bool cc_changed = (cc_state != _last_cc_adjust_state);
    bool value_changed = (current_ma != _last_adjust_value);

    // Skip redraw if nothing changed
    if (!_needs_full_redraw && !value_changed && !cc_changed) {
        return;
    }

    int y = CONTENT_Y_START + 40;

    // Only clear content area on full redraw
    if (_needs_full_redraw) {
        hw.display.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START - 40, UIColors::BACKGROUND);
    }

    // Compute layout positions (needed for both value and badge)
    char buf[32];
    snprintf(buf, sizeof(buf), "%5.2f", current_ma / 1000.0f);
    const int A_WIDTH = ST7789::getStringWidthAA("A", FONT_LARGE);
    int value_width = ST7789::getStringWidthAA(buf, FONT_LARGE);
    const int GAP = 4;
    int total_w = value_width + GAP + A_WIDTH;
    int value_x = (SCREEN_WIDTH - total_w) / 2;
    int unit_x = value_x + value_width + GAP;

    // Only redraw value+unit when the current value changed (avoids flicker on badge toggle)
    if (value_changed || _needs_full_redraw) {
        _last_adjust_value = current_ma;
        hw.display.fillRect(value_x - 20, y, total_w + 40, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
        hw.display.drawStringAA(value_x, y, buf, UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);
        hw.display.drawStringAA(unit_x, y, "A", UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);
    }

    // CC/OCP mode indicator badge — only redraw when badge state changes
    if (cc_changed || _needs_full_redraw) {
        const int BADGE_H = 16;
        const int MODE_X = unit_x + A_WIDTH + 6;
        const int MODE_Y = y + (FONT_LARGE->lineHeight - BADGE_H) / 2;
        const char* mode_text = cc_enabled ? "CC" : "OCP";
        uint16_t badge_color = cc_enabled ? UIColors::ACCENT : UIColors::MUTED;
        int ocp_w = ST7789::getStringWidthAA("OCP", FONT_SMALL) + 8;
        hw.display.fillRect(MODE_X - 1, MODE_Y - 1, ocp_w + 2, BADGE_H + 2, UIColors::BACKGROUND);
        int badge_w = ST7789::getStringWidthAA(mode_text, FONT_SMALL) + 8;
        int badge_x = MODE_X + (ocp_w - badge_w) / 2;
        hw.display.fillRoundRect(badge_x, MODE_Y, badge_w, BADGE_H, 3, badge_color);
        int text_x = badge_x + (badge_w - ST7789::getStringWidthAA(mode_text, FONT_SMALL)) / 2;
        int text_y = MODE_Y + (BADGE_H - FONT_SMALL->lineHeight) / 2;
        hw.display.drawStringAA(text_x, text_y, mode_text, UIColors::BACKGROUND, badge_color, FONT_SMALL);
        _last_cc_adjust_state = cc_state;
    }

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
        snprintf(min_str, sizeof(min_str), "%.2fA", AppConfig::CURRENT_LIMIT_MIN_MA / 1000.0f);
        snprintf(max_str, sizeof(max_str), "%.1fA (max)", max_ma / 1000.0f);

        hw.display.drawStringAA(MARGIN * 2, y, min_str, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);

        // Right-align max
        int max_width = ST7789::getStringWidthAA(max_str, FONT_SMALL);
        hw.display.drawStringAA(SCREEN_WIDTH - MARGIN * 2 - max_width, y, max_str,
                              UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);

        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 35,
                              "Rotate: Adjust  BTN2: CC/OCP", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
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
    snprintf(buf, sizeof(buf), "%5.2f", target_mv / 1000.0f);

    // Fixed layout: center point at screen middle, V after the number area
    const int UNIT_X = (SCREEN_WIDTH / 2) + 42;  // Fixed position for "V"
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

void DisplayManager::drawAvsVoltageAdjust() {
    uint32_t target_mv = stateMachine.getAvsTargetVoltageMv();
    uint32_t min_mv = stateMachine.getAvsMinVoltageMv();
    uint32_t max_mv = stateMachine.getAvsMaxVoltageMv();
    uint32_t max_current = stateMachine.getAvsMaxCurrentMa();

    // Skip redraw if value hasn't changed
    if (!_needs_full_redraw && target_mv == _last_avs_voltage) {
        return;
    }
    _last_avs_voltage = target_mv;

    int y = CONTENT_Y_START + 20;

    // Only clear content area on full redraw
    if (_needs_full_redraw) {
        hw.display.fillRect(0, CONTENT_Y_START, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_Y_START - 40, UIColors::BACKGROUND);
        drawCenteredStringAA(y, "Adjustable Voltage", UIColors::SYNAPTICON_PINK, FONT_SMALL);
        y += 18;
    } else {
        y += 18;
    }

    // Draw target voltage - large display with V at fixed position
    y += 10;
    char buf[32];
    snprintf(buf, sizeof(buf), "%5.2f", target_mv / 1000.0f);

    const int UNIT_X = (SCREEN_WIDTH / 2) + 42;
    const int VALUE_RIGHT = UNIT_X - 8;
    int value_width = ST7789::getStringWidthAA(buf, FONT_LARGE);
    int value_x = VALUE_RIGHT - value_width;

    hw.display.fillRect(value_x - 20, y, VALUE_RIGHT - value_x + 20, FONT_LARGE->lineHeight, UIColors::BACKGROUND);
    hw.display.drawStringAA(value_x, y, buf, UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);
    hw.display.drawStringAA(UNIT_X, y, "V", UIColors::ACCENT, UIColors::BACKGROUND, FONT_LARGE);

    // Draw progress bar (scaled to AVS range)
    y += 50;
    uint32_t range = max_mv - min_mv;
    uint8_t percent = (range > 0)
        ? ((target_mv - min_mv) * 100) / range
        : 0;
    drawProgressBar(MARGIN * 2, y, SCREEN_WIDTH - MARGIN * 4, 20, percent, UIColors::SYNAPTICON_PINK);

    // Draw min/max labels and hints on full redraw only
    if (_needs_full_redraw) {
        y += 30;
        char min_str[16], max_str[16];
        snprintf(min_str, sizeof(min_str), "%.1fV", min_mv / 1000.0f);
        snprintf(max_str, sizeof(max_str), "%.1fV", max_mv / 1000.0f);

        hw.display.drawStringAA(MARGIN * 2, y, min_str, UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
        int max_width = ST7789::getStringWidthAA(max_str, FONT_SMALL);
        hw.display.drawStringAA(SCREEN_WIDTH - MARGIN * 2 - max_width, y, max_str,
                              UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);

        y += 25;
        snprintf(buf, sizeof(buf), "Max current: %umA", (unsigned)max_current);
        drawCenteredStringAA(y, buf, UIColors::TEXT_SECONDARY, FONT_SMALL);

        hw.display.drawStringAA(MARGIN, SCREEN_HEIGHT - 35,
                              "Rotate: 25mV steps", UIColors::TEXT_SECONDARY, UIColors::BACKGROUND, FONT_SMALL);
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

    // Snapshot current values
    bool auto_pps = settings.isAutoPpsEnabled();
    bool auto_avs = settings.isAutoAvsEnabled();
    bool auto_output = settings.isAutoOutput();
    bool sounds = settings.isSoundsEnabled();
    uint8_t brightness = stateMachine.getBrightnessValue();
    bool brightness_adj = stateMachine.isBrightnessAdjusting();
    uint8_t dim_val = stateMachine.getDimTimeoutValue();
    bool dim_adj = stateMachine.isDimTimeoutAdjusting();
    uint8_t mel_val = stateMachine.getMelodyValue();
    bool mel_adj = stateMachine.isMelodyAdjusting();
    uint8_t contract_mode = stateMachine.getContractModeValue();
    bool contract_adj = stateMachine.isContractModeAdjusting();

    // Helper function to get contract mode name
    auto getContractModeName = [](uint8_t mode) -> const char* {
        switch (mode) {
            case 0: return "Lowest V";
            case 1: return "Highest V";
            case 2: return "Last";
            default: return "Unknown";
        }
    };

    // Helper: check if item at given index needs redraw due to selection change
    // Only the previously-selected and newly-selected items need highlight update
    bool sel_changed = (sel_idx != _last_settings_selection);
    auto sel_affects = [&](int idx) {
        return sel_changed && (idx == sel_idx || idx == _last_settings_selection);
    };

    // Y positions for each item
    int y_base = CONTENT_Y_START + 5;
    auto y_for = [&](int idx) { return y_base + idx * MENU_ITEM_HEIGHT; };

    // Item 0: Flash EEPROM
    if (_needs_full_redraw || sel_affects(0)) {
        drawMenuItem(y_for(0), "Flash EEPROM", selected == SettingsItem::FLASH_EEPROM);
    }

    // Item 1: Auto PPS toggle
    if (_needs_full_redraw || sel_affects(1) || auto_pps != _last_auto_pps) {
        drawSettingsItem(y_for(1), "Auto PPS tuning", auto_pps,
                        selected == SettingsItem::AUTO_PPS, true);
    }

    // Item 2: Auto AVS toggle
    if (_needs_full_redraw || sel_affects(2) || auto_avs != _last_auto_avs) {
        drawSettingsItem(y_for(2), "Auto AVS tuning", auto_avs,
                        selected == SettingsItem::AUTO_AVS, true);
    }

    // Item 3: Auto Output toggle
    if (_needs_full_redraw || sel_affects(3) || auto_output != _last_auto_output) {
        drawSettingsItem(y_for(3), "Auto Output EN", auto_output,
                        selected == SettingsItem::AUTO_OUTPUT, true);
    }

    // Item 4: Brightness
    if (_needs_full_redraw || sel_affects(4) || brightness != _last_brightness_value ||
        brightness_adj != _last_brightness_adjusting) {
        drawBrightnessItem(y_for(4), selected == SettingsItem::BRIGHTNESS);
    }

    // Item 5: Dim timeout
    if (_needs_full_redraw || sel_affects(5) || dim_val != _last_dim_timeout ||
        dim_adj != _last_dim_adjusting) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d min", dim_val);
        drawValueAdjustItem(y_for(5), "Auto Dim timeout:", buf, selected == SettingsItem::DIM_TIMEOUT, dim_adj);
    }

    // Item 6: Startup melody
    if (_needs_full_redraw || sel_affects(6) || mel_val != _last_melody ||
        mel_adj != _last_melody_adjusting) {
        drawValueAdjustItem(y_for(6), "Melody:", getStartupMelodyName(mel_val),
                           selected == SettingsItem::STARTUP_MELODY, mel_adj);
    }

    // Item 7: Startup contract mode
    if (_needs_full_redraw || sel_affects(7) || contract_mode != _last_contract_mode ||
        contract_adj != _last_contract_mode_adjusting) {
        drawValueAdjustItem(y_for(7), "Startup V:", getContractModeName(contract_mode),
                           selected == SettingsItem::STARTUP_CONTRACT, contract_adj);
    }

    // Item 8: Sounds toggle
    if (_needs_full_redraw || sel_affects(8) || sounds != _last_sounds) {
        drawSettingsItem(y_for(8), "Sounds", sounds,
                        selected == SettingsItem::SOUNDS, true);
    }

    // Item 9: Back
    if (_needs_full_redraw || sel_affects(9)) {
        drawMenuItemMuted(y_for(9), "Back", selected == SettingsItem::BACK);
    }

    // Update all tracking variables
    _last_settings_selection = sel_idx;
    _last_auto_pps = auto_pps;
    _last_auto_avs = auto_avs;
    _last_auto_output = auto_output;
    _last_sounds = sounds;
    _last_brightness_value = brightness;
    _last_brightness_adjusting = brightness_adj;
    _last_dim_timeout = dim_val;
    _last_dim_adjusting = dim_adj;
    _last_melody = mel_val;
    _last_melody_adjusting = mel_adj;
    _last_contract_mode = contract_mode;
    _last_contract_mode_adjusting = contract_adj;

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

void DisplayManager::drawValueAdjustItem(int y, const char* label, const char* value, bool selected, bool adjusting) {
    uint16_t bg = selected ? UIColors::HIGHLIGHT_BG : UIColors::BACKGROUND;
    uint16_t fg = selected ? UIColors::HIGHLIGHT_FG : UIColors::TEXT_PRIMARY;

    hw.display.fillRect(MARGIN, y, SCREEN_WIDTH - MARGIN * 2, MENU_ITEM_HEIGHT - 2, bg);

    if (selected && adjusting) {
        hw.display.drawStringAA(MARGIN + 5, y + 5, "<", fg, bg, FONT_SMALL);
    } else {
        hw.display.drawStringAA(MARGIN + 5, y + 5, selected ? ">" : " ", fg, bg, FONT_SMALL);
    }

    hw.display.drawStringAA(MARGIN + 20, y + 5, label, fg, bg, FONT_SMALL);

    int value_x = SCREEN_WIDTH - MARGIN - ST7789::getStringWidthAA(value, FONT_SMALL) - 10;
    hw.display.drawStringAA(value_x, y + 5, value, fg, bg, FONT_SMALL);

    if (selected && adjusting) {
        hw.display.drawStringAA(SCREEN_WIDTH - MARGIN - 10, y + 5, ">", fg, bg, FONT_SMALL);
    }
}

void DisplayManager::drawEepromFlashScreen() {
    uint8_t stage = static_cast<uint8_t>(tpsEepromWorkflow.getStage());
    uint8_t phase = tpsEepromWorkflow.getPhase();
    uint8_t progress = tpsEepromWorkflow.getProgress();
    bool result = tpsEepromWorkflow.getResult();
    bool confirm_yes = tpsEepromWorkflow.isConfirmYes();
    const char* message = tpsEepromWorkflow.getMessage();

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
    const int LINE_H = 17;

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
    const int LABEL_X = MARGIN * 3;
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
    hw.display.drawStringAA(VALUE_X, y, Version::GITHUB_SHORT, UIColors::LINK_BLUE, UIColors::BACKGROUND, FONT_SMALL);
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
            break;

        case FaultType::OVERTEMPERATURE:
            fault_name = "OVERTEMPERATURE";
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

    // Overtemperature: vertically aligned columns for labels, colons, values, and °C
    if (fault == FaultType::OVERTEMPERATURE) {
        char temp_buf[16];
        int tw;

        // Row 1: Trigger temperature (frozen at fault time)
        hw.display.drawStringAA(OT_LABEL_X, y, "Trigger", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OT_COLON_X, y, ":", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        snprintf(temp_buf, sizeof(temp_buf), "%5.1f", safety.getState().max_temperature_c);
        hw.display.drawStringAA(OT_VALUE_X, y, temp_buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        tw = ST7789::getStringWidthAA(temp_buf, FONT_SMALL);
        if (OT_VALUE_X + tw < OT_UNIT_X)
            hw.display.fillRect(OT_VALUE_X + tw, y, OT_UNIT_X - OT_VALUE_X - tw, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
        hw.display.drawStringAA(OT_UNIT_X, y - 3, "o", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OT_UNIT_X + 6, y, "C", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        y += OT_ROW_H;

        // Row 2: Limit temperature
        hw.display.drawStringAA(OT_LABEL_X, y, "Limit", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OT_COLON_X, y, ":", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        snprintf(temp_buf, sizeof(temp_buf), "%5d", AppConfig::TEMP_SHUTDOWN_C);
        hw.display.drawStringAA(OT_VALUE_X, y, temp_buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        tw = ST7789::getStringWidthAA(temp_buf, FONT_SMALL);
        if (OT_VALUE_X + tw < OT_UNIT_X)
            hw.display.fillRect(OT_VALUE_X + tw, y, OT_UNIT_X - OT_VALUE_X - tw, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
        hw.display.drawStringAA(OT_UNIT_X, y - 3, "o", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OT_UNIT_X + 6, y, "C", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        y += OT_ROW_H;

        // Row 3: Now (live temperature) - label, colon, and °C drawn here; value updated by drawFaultLiveTemperature()
        hw.display.drawStringAA(OT_LABEL_X, y, "Now", UIColors::WARNING, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OT_COLON_X, y, ":", UIColors::WARNING, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OT_UNIT_X, y - 3, "o", UIColors::WARNING, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OT_UNIT_X + 6, y, "C", UIColors::WARNING, UIColors::BACKGROUND, FONT_SMALL);
        _fault_now_temp_y = y;  // Save Y for live temperature updates
        y += OT_ROW_H + 5;
    } else if (fault == FaultType::OVERCURRENT) {
        char buf[16];

        // Row 1: Limit value (user configured)
        hw.display.drawStringAA(OC_LABEL_X+34, y, "Limit", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OC_COLON_X, y, ":", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        snprintf(buf, sizeof(buf), "%5.2f", stateMachine.getFaultLimitValue());
        hw.display.drawStringAA(OC_VALUE_X, y, buf, UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        hw.display.drawStringAA(OC_UNIT_X, y, "A", UIColors::TEXT_PRIMARY, UIColors::BACKGROUND, FONT_SMALL);
        y += OC_ROW_H;
    } else {
        if (detail1[0]) {
            drawCenteredStringAA(y, detail1, UIColors::TEXT_PRIMARY, FONT_SMALL);
            y += 20;
        }
        if (detail2[0]) {
            drawCenteredStringAA(y, detail2, UIColors::TEXT_PRIMARY, FONT_SMALL);
            y += 20;
        }
    }

    y += 20;
    drawCenteredStringAA(y, "Load switch disabled", UIColors::WARNING, FONT_SMALL);

    // Draw acknowledge hint
    drawCenteredStringAA(SCREEN_HEIGHT - 30, "[Press knob to acknowledge]", UIColors::TEXT_SECONDARY, FONT_SMALL);
}

void DisplayManager::drawFaultLiveTemperature() {
    // Live-update only the temperature value — label, colon, and °C are drawn by drawFaultDetails()
    const int y = _fault_now_temp_y;

    char buf[16];
    snprintf(buf, sizeof(buf), "%5.1f", safety.getState().max_temperature_c);
    hw.display.drawStringAA(OT_VALUE_X, y, buf, UIColors::WARNING, UIColors::BACKGROUND, FONT_SMALL);

    // Gap-fill between value and °C unit
    int num_w = ST7789::getStringWidthAA(buf, FONT_SMALL);
    if (OT_VALUE_X + num_w < OT_UNIT_X)
        hw.display.fillRect(OT_VALUE_X + num_w, y, OT_UNIT_X - OT_VALUE_X - num_w, FONT_SMALL->lineHeight, UIColors::BACKGROUND);
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
