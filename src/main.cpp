#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware.h"
#include "utils/logging.h"

// // Global GPIO Interrupt Handler
// void gpio_callback(uint gpio, uint32_t events) {
//     if (hw.encoder.isMyPin(gpio)) {
//         hw.encoder.handleISR(gpio, events);
//     }
// }

void over_current(uint gpio, uint32_t events) {
    // 1. Immediately cut power (Safety Critical)
    hw.loadSwitch.off();
    
    // 2. Visual indication
    hw.rgbLed.setColor(255, 0, 0, 255); // Red full brightness
    
    // 3. Optional: Read INA228 Diagnose register to clear the Latch
    // Note: Doing I2C inside an ISR is generally bad practice because it's slow.
    // Ideally, set a flag here and handle the I2C clear in the main loop.
    printf("!! OVERCURRENT DETECTED - LOAD DISABLED !!\n");
}

// Global GPIO Router
void gpio_callback(uint gpio, uint32_t events) {
    // Handle Encoder
    if (hw.encoder.isMyPin(gpio)) {
        hw.encoder.handleISR(gpio, events);
    }
    
    // Handle Overcurrent
    if (gpio == Board::PIN_SWITCH_EN_READ) {
        over_current(gpio, events);
    }
}

int main() {
    // ========================================
    // PHASE 1 TEST CODE - Hardware Foundation
    // ========================================

    LOG_SEPARATOR();
    LOG_INFO("PD240W Phase 1 Hardware Test");
    LOG_SEPARATOR();

    // 1. Initialize Hardware
    hw.init();

    // 2. Setup GPIO Interrupts
    gpio_set_irq_enabled_with_callback(Board::PIN_ENC_A, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &gpio_callback);
    gpio_set_irq_enabled(Board::PIN_ENC_B, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    gpio_set_irq_enabled(Board::PIN_SWITCH_EN_READ, GPIO_IRQ_EDGE_FALL, true);

    LOG_INFO("All interrupts enabled");

    // 3. Play Mario Power-Up melody to test buzzer
    LOG_INFO("Playing startup melody...");
    extern const Note MARIO_POWERUP[];
    extern const uint8_t MARIO_POWERUP_LENGTH;
    hw.buzzer.playMelody(MARIO_POWERUP, MARIO_POWERUP_LENGTH);

    // 4. Test LCD text rendering
    hw.display.fillScreen(ST7789::COLOR_BLACK);
    hw.display.drawString(10, 10, "PD240W Phase 1", ST7789::COLOR_WHITE, ST7789::COLOR_BLACK, 2);
    hw.display.drawString(10, 40, "Hardware Test", ST7789::COLOR_CYAN, ST7789::COLOR_BLACK, 2);
    hw.display.drawLine(10, 70, 230, 70, ST7789::COLOR_GREEN);

    LOG_INFO("Display initialized - showing header");

    // Test state tracking
    static int last_encoder_ticks = 0;
    static bool melody_test_done = false;

    // Timers for non-blocking updates
    absolute_time_t next_sensor_read = make_timeout_time_ms(1000);
    absolute_time_t next_power_read = make_timeout_time_ms(500);

    LOG_SEPARATOR();
    LOG_INFO("Entering main loop - press buttons to test!");
    LOG_INFO("BTN1: Toggle debug LED | BTN2: Test buzzer | ENC BTN: Reset encoder");
    LOG_SEPARATOR();

    while (true) {
        // ===== Button Tests =====

        // BTN1: Toggle debug LED and test SimpleIO read()
        if (hw.btn1.isClicked()) {
            hw.debugLed.toggle();
            bool led_state = hw.debugLed.read();
            LOG_INFO("BTN1 clicked - Debug LED: %s", led_state ? "ON" : "OFF");

            // Update display
            hw.display.fillRect(10, 90, 220, 20, ST7789::COLOR_BLACK);
            hw.display.drawString(10, 90, "BTN1: LED ", ST7789::COLOR_YELLOW, ST7789::COLOR_BLACK, 1);
            hw.display.drawString(80, 90, led_state ? "ON" : "OFF", ST7789::COLOR_GREEN, ST7789::COLOR_BLACK, 1);
        }

        // BTN2: Play test tone
        if (hw.btn2.isClicked()) {
            LOG_INFO("BTN2 clicked - Playing test tone");
            hw.buzzer.playTone(1000, 200);

            hw.display.fillRect(10, 110, 220, 20, ST7789::COLOR_BLACK);
            hw.display.drawString(10, 110, "BTN2: Buzzer Test", ST7789::COLOR_YELLOW, ST7789::COLOR_BLACK, 1);
        }

        // Encoder Button: Toggle output switch and clear INA228 fault if needed
        if (hw.btnEnc.isClicked()) {
            // Clear INA228 latched fault by reading diagnose register
            hw.powerMonitor.getDiagnoseAlert();

            // Toggle load switch
            bool current_state = hw.loadSwitch.read();
            if (current_state) {
                hw.loadSwitch.off();
                hw.rgbLed.setColor(255, 255, 0, 100); // Yellow - output disabled
                LOG_INFO("Encoder button: Output DISABLED");
            } else {
                hw.loadSwitch.on();
                hw.rgbLed.setColor(0, 255, 0, 100); // Green - output enabled
                LOG_INFO("Encoder button: Output ENABLED");
            }

            hw.display.fillRect(10, 130, 220, 20, ST7789::COLOR_BLACK);
            hw.display.drawString(10, 130, current_state ? "Output: OFF" : "Output: ON",
                                  current_state ? ST7789::COLOR_RED : ST7789::COLOR_GREEN,
                                  ST7789::COLOR_BLACK, 1);
        }

        // ===== Encoder Test (with debouncing) =====
        int current_ticks = hw.encoder.getTicks();
        if (current_ticks != last_encoder_ticks) {
            LOG_DEBUG("Encoder ticks: %d (delta: %+d)", current_ticks, current_ticks - last_encoder_ticks);
            last_encoder_ticks = current_ticks;

            // Display encoder value
            hw.display.fillRect(10, 160, 220, 30, ST7789::COLOR_BLACK);
            hw.display.drawString(10, 160, "Encoder:", ST7789::COLOR_CYAN, ST7789::COLOR_BLACK, 2);
            hw.display.drawInt(140, 160, current_ticks, ST7789::COLOR_WHITE, ST7789::COLOR_BLACK, 2);
        }

        // ===== ADC Sensor Readings =====
        if (absolute_time_diff_us(get_absolute_time(), next_sensor_read) < 0) {
            float vbus_pre = hw.adc.getVBUS();
            float temperature = hw.adc.getTemperature();

            LOG_VALUE("VBUS (pre-switch)", vbus_pre, "V");
            LOG_VALUE("Temperature", temperature, "°C");

            // Display ADC readings
            hw.display.fillRect(10, 200, 220, 50, ST7789::COLOR_BLACK);
            hw.display.drawString(10, 200, "ADC Readings:", ST7789::COLOR_GREEN, ST7789::COLOR_BLACK, 1);

            hw.display.drawString(10, 220, "VBUS:", ST7789::COLOR_WHITE, ST7789::COLOR_BLACK, 1);
            hw.display.drawFloat(70, 220, vbus_pre, 2, ST7789::COLOR_CYAN, ST7789::COLOR_BLACK, 1);
            hw.display.drawString(130, 220, "V", ST7789::COLOR_CYAN, ST7789::COLOR_BLACK, 1);

            hw.display.drawString(10, 235, "Temp:", ST7789::COLOR_WHITE, ST7789::COLOR_BLACK, 1);
            hw.display.drawFloat(70, 235, temperature, 1, ST7789::COLOR_YELLOW, ST7789::COLOR_BLACK, 1);
            hw.display.drawString(130, 235, "C", ST7789::COLOR_YELLOW, ST7789::COLOR_BLACK, 1);

            next_sensor_read = make_timeout_time_ms(1000);
        }

        // ===== INA228 Power Monitor =====
        if (absolute_time_diff_us(get_absolute_time(), next_power_read) < 0) {
            float voltage = hw.powerMonitor.getBusVoltage();
            float current = hw.powerMonitor.getCurrent();
            float power = hw.powerMonitor.getPower();

            LOG_INFO("INA228 - V: %.3fV | I: %.3fA | P: %.3fW", voltage, current, power);

            // Display power monitoring
            hw.display.fillRect(10, 260, 220, 50, ST7789::COLOR_BLACK);
            hw.display.drawString(10, 260, "INA228 (post-sw):", ST7789::COLOR_MAGENTA, ST7789::COLOR_BLACK, 1);

            hw.display.drawFloat(10, 280, voltage, 2, ST7789::COLOR_CYAN, ST7789::COLOR_BLACK, 1);
            hw.display.drawString(60, 280, "V", ST7789::COLOR_CYAN, ST7789::COLOR_BLACK, 1);

            hw.display.drawFloat(80, 280, current, 3, ST7789::COLOR_GREEN, ST7789::COLOR_BLACK, 1);
            hw.display.drawString(140, 280, "A", ST7789::COLOR_GREEN, ST7789::COLOR_BLACK, 1);

            hw.display.drawFloat(10, 295, power, 2, ST7789::COLOR_YELLOW, ST7789::COLOR_BLACK, 1);
            hw.display.drawString(60, 295, "W", ST7789::COLOR_YELLOW, ST7789::COLOR_BLACK, 1);

            next_power_read = make_timeout_time_ms(500);
        }

        // ===== RGB LED Update =====
        hw.rgbLed.update();

        // ===== Melody completion test =====
        if (!melody_test_done && !hw.buzzer.isPlayingMelody()) {
            melody_test_done = true;
            LOG_INFO("Startup melody completed successfully");
        }
    }

    return 0;
}
