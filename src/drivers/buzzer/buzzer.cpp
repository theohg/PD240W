#include "buzzer.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include <stdint.h>

Buzzer::Buzzer(uint pin) {
    _pin = pin;
    _current_wrap = 0;
    _alarm_id = 0;
    _playing_melody = false;
    _melody = nullptr;
    _melody_length = 0;
    _melody_index = 0;
}

void Buzzer::init() {
    // 1. Set function to PWM
    gpio_set_function(_pin, GPIO_FUNC_PWM);
    
    // 2. Cache slice and channel (saves CPU cycles later)
    _slice_num = pwm_gpio_to_slice_num(_pin);
    _channel = pwm_gpio_to_channel(_pin);

    // 3. Set a default config but DO NOT enable yet to avoid popping sounds
    pwm_config config = pwm_get_default_config();
    pwm_init(_slice_num, &config, false); 
}

bool Buzzer::setFrequency(uint32_t frequency) {
    if (frequency == 0) return false;

    // Get the actual system clock frequency (usually 125MHz)
    uint32_t sys_clock = clock_get_hz(clk_sys);
    
    // To get a generic buzzer range (100Hz - 10kHz), a divider of 16.0 works well.
    // It allows precise frequencies without overflowing the 16-bit wrap counter.
    float clkdiv = 16.0f; 
    
    // Calculate the wrap value: (F_sys / (div * F_target)) - 1
    // The -1 is because the counter is 0-indexed
    uint32_t wrap = static_cast<uint32_t>((sys_clock / (clkdiv * frequency)) - 1);

    // RP2040 Wrap limit is 16-bit (65535). If wrap is too high, we need a larger divider.
    if (wrap > 65535) {
        wrap = 65535;
    }
    
    _current_wrap = wrap; // Store for duty cycle calculation

    pwm_set_clkdiv(_slice_num, clkdiv);
    pwm_set_wrap(_slice_num, wrap);
    
    // Enable PWM now that setup is complete
    pwm_set_enabled(_slice_num, true);
    
    return true;
}

void Buzzer::setDutyCycle(uint16_t duty_cycle) {
    // Math: Map the 0-65535 input to the 0-wrap range
    // We cast to uint32_t to prevent overflow during multiplication
    uint32_t level = (static_cast<uint32_t>(duty_cycle) * _current_wrap) / 65535;
    
    pwm_set_chan_level(_slice_num, _channel, level);
}

void Buzzer::stop() {
    pwm_set_chan_level(_slice_num, _channel, 0);
    // Note: We don't disable PWM completely, just set volume to 0.
    // This prevents "popping" noises when starting the next note.
}

// The static function that the Timer calls
int64_t Buzzer::stopToneCallback(alarm_id_t id, void *user_data) {
    // Cast the generic pointer back to our specific Buzzer object
    Buzzer *buzzer = static_cast<Buzzer*>(user_data);
    
    // Stop the sound
    buzzer->stop();
    
    // Reset alarm ID indicating no alarm is running
    buzzer->_alarm_id = 0;
    
    return 0; // Return 0 to stop the alarm from repeating
}

void Buzzer::playTone(uint32_t frequency, uint32_t duration_ms) {
    // 1. If a previous note is still playing (alarm pending), cancel it!
    // This prevents the old "stop" command from cutting off our NEW note.
    if (_alarm_id > 0) {
        cancel_alarm(_alarm_id);
    }

    // 2. Start the sound
    setFrequency(frequency);
    setDutyCycle(32768); // 50% Duty Cycle (Standard square wave beep)

    // 3. Set a timer to turn it off later
    // We pass 'this' so the static callback knows WHICH buzzer to stop
    _alarm_id = add_alarm_in_ms(duration_ms, stopToneCallback, this, true);
}

// ===== Melody Playback =====

// Mario Power-Up melody: E5, G5, C6, E6, G6, C7, E7, G7
const Note MARIO_POWERUP[] = {
    {659, 125},   // E5
    {784, 125},   // G5
    {1047, 125},  // C6
    {1319, 125},  // E6
    {1568, 125},  // G6
    {2093, 125},  // C7
    {2637, 125},  // E7
    {3136, 500}   // G7 (longer final note)
};

const uint8_t MARIO_POWERUP_LENGTH = sizeof(MARIO_POWERUP) / sizeof(Note);

// Callback for playing next note in melody
int64_t Buzzer::playNextNoteCallback(alarm_id_t id, void *user_data) {
    Buzzer *buzzer = static_cast<Buzzer*>(user_data);

    // Check if we've finished the melody
    if (buzzer->_melody_index >= buzzer->_melody_length) {
        buzzer->stop();
        buzzer->_playing_melody = false;
        buzzer->_alarm_id = 0;
        return 0; // Stop the alarm
    }

    // Get current note
    const Note& note = buzzer->_melody[buzzer->_melody_index];
    buzzer->_melody_index++;

    // Play the note (or rest if frequency is 0)
    if (note.frequency > 0) {
        buzzer->setFrequency(note.frequency);
        buzzer->setDutyCycle(32768); // 50% duty cycle
    } else {
        buzzer->stop(); // Rest (silence)
    }

    // Schedule next note after this note's duration
    buzzer->_alarm_id = add_alarm_in_ms(note.duration, playNextNoteCallback, buzzer, true);

    return 0;
}

void Buzzer::playMelody(const Note* melody, uint8_t length) {
    // Cancel any currently playing tone or melody
    if (_alarm_id > 0) {
        cancel_alarm(_alarm_id);
    }

    // Setup melody state
    _melody = melody;
    _melody_length = length;
    _melody_index = 0;
    _playing_melody = true;

    // Start playing first note
    playNextNoteCallback(0, this);
}

void Buzzer::stopMelody() {
    if (_alarm_id > 0) {
        cancel_alarm(_alarm_id);
        _alarm_id = 0;
    }
    stop();
    _playing_melody = false;
}