#include "sk6812.h"

SK6812::SK6812(uint pin, PIO pio, bool is_rgbw) {
    _pin = pin;
    _pio = pio;
    _is_rgbw = is_rgbw;
    _sm = -1; // Uninitialized

    _brightness = 255; 
    _stored_r = 0; _stored_g = 0; _stored_b = 0;
    _is_blinking = false;
    _blink_state_on = true;
    _last_blink_time = 0;
    _blink_duration_ms = 0;
}

bool SK6812::init() {
    // 1. Load the program into PIO memory
    // Note: pio_add_program checks if it's already added to avoid duplication
    if (!pio_can_add_program(_pio, &sk6812_program)) {
        return false; // Not enough memory
    }
    _offset = pio_add_program(_pio, &sk6812_program);

    // 2. Claim a free state machine (safest way)
    _sm = pio_claim_unused_sm(_pio, true);
    if (_sm == -1) return false; // No SM available

    // 3. Configure via the helper generated in the .pio.h file
    sk6812_program_init(_pio, _sm, _offset, _pin, 800000, _is_rgbw);
    
    return true;
}

// Internal helper to handle Brightness + Bit Shifting + PIO Write
void SK6812::_write(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t data;

    // Apply brightness scaling
    // (val * brightness) / 255
    // Note: We cast to uint16_t to prevent overflow before division
    uint8_t r_out = ((uint16_t)r * _brightness) / 255;
    uint8_t g_out = ((uint16_t)g * _brightness) / 255;
    uint8_t b_out = ((uint16_t)b * _brightness) / 255;

    // GRB order for WS2812/SK6812 RGB
    // Shifted to top 24 bits for PIO
    uint32_t grb = ((uint32_t)g_out << 16) | 
                    ((uint32_t)r_out << 8)  | 
                    ((uint32_t)b_out);
    data = grb << 8;

    pio_sm_put_blocking(_pio, _sm, data);
}

// void SK6812::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
//     uint32_t data;

//     // SK6812/WS2812 usually expect GRB data order
//     if (_is_rgbw) {
//         // For RGBW, we usually send: Green, Red, Blue, White (32 bits)
//         // Adjust this order if your specific LED is different (e.g. WRGB)
//         data = ((uint32_t)g << 24) | 
//                ((uint32_t)r << 16) | 
//                ((uint32_t)b << 8)  | 
//                ((uint32_t)w);
//     } else {
//         // For RGB, we send 24 bits: Green, Red, Blue
//         // But the PIO shifts Left (MSB first). 
//         // We must shift our 24-bit data to the top 8 bits of the 32-bit FIFO word.
//         uint32_t grb = ((uint32_t)g << 16) | 
//                        ((uint32_t)r << 8)  | 
//                        ((uint32_t)b);
                       
//         data = grb << 8;
//     }

//     // Write to the FIFO
//     pio_sm_put_blocking(_pio, _sm, data);
// }

void SK6812::setColor(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    // 1. Store the "Target" color
    _stored_r = r; 
    _stored_g = g; 
    _stored_b = b; 

    setBrightness(brightness);

    // 2. If not blinking, update immediate. 
    // If blinking, the update() loop will handle it on next cycle.
    if (!_is_blinking) {
        _write(_stored_r, _stored_g, _stored_b);
    }
}

void SK6812::setColor(LedColor color, uint8_t brightness) {
    switch (color) {
        case LedColor::OFF:     setColor(0, 0, 0, 0);               return;
        case LedColor::RED:     setColor(255, 0, 0, brightness);    return;
        case LedColor::GREEN:   setColor(0, 255, 0, brightness);    return;
        case LedColor::BLUE:    setColor(0, 0, 255, brightness);    return;
        case LedColor::YELLOW:  setColor(255, 255, 0, brightness);  return;
        case LedColor::ORANGE:  setColor(255, 120, 0, brightness);  return;
        case LedColor::MAGENTA: setColor(255, 0, 255, brightness);  return;
    }
}

void SK6812::setBrightness(uint8_t b) {
    _brightness = b;
    
    // Refresh the LED immediately with new brightness
    if (!_is_blinking) {
        _write(_stored_r, _stored_g, _stored_b);
    }
}

void SK6812::off() {
    if (_is_blinking) stopBlink();
    _write(0, 0, 0);
}

// --- Non-Blocking Blink Logic ---

void SK6812::startBlink(uint32_t interval_ms, uint32_t duration_ms) {
    _blink_interval_ms = interval_ms;
    _blink_duration_ms = duration_ms; // Store the duration constraint
    
    _is_blinking = true;
    _blink_state_on = true;
    
    uint32_t now = to_ms_since_boot(get_absolute_time());
    _last_blink_time = now;
    _blink_start_time = now; // Mark when blinking started
    
    // Immediately show ON state
    _write(_stored_r, _stored_g, _stored_b);
}

void SK6812::stopBlink() {
    _is_blinking = false;
    _blink_duration_ms = 0;
    _write(0, 0, 0);
}

void SK6812::update() {
    if (!_is_blinking) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // 1. Check for Duration Expiration
    // If duration is NOT 0, check if we exceeded it
    if (_blink_duration_ms > 0) {
        if ((now - _blink_start_time) >= _blink_duration_ms) {
            stopBlink();
            return;
        }
    }

    // 2. Check for Blink Toggle
    if (now - _last_blink_time >= _blink_interval_ms) {
        _blink_state_on = !_blink_state_on;
        _last_blink_time = now;

        if (_blink_state_on) {
            _write(_stored_r, _stored_g, _stored_b);
        } else {
            _write(0, 0, 0);
        }
    }
}