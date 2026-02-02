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

// Mario Power-Up melody (Super Mario Bros. mushroom sound)
// Based on the classic NES sound: ascending arpeggio E-G-E-C-D-G pattern
// Frequencies are in the 4th and 5th octave range for a pleasant tone
const Note MARIO_POWERUP[] = {
    {196, 70},   // G3
    {247, 70},   // B3
    {294, 70},   // D4
    {392, 70},   // G4
    {494, 70},   // B4
    {587, 70},   // D5
    {784, 70},   // G5
    {988, 70},   // B5
    {1175, 70},  // D6
    {1568, 70},  // G6
    {1976, 70},  // B6
    {2349, 70},  // D7
    {3136, 70}   // G7
};
// const Note MARIO_ONEUP[] = {
//     {659, 75},   // E5
//     {784, 75},   // G5
//     {1319, 75},  // E6 (Jumps up high)
//     {1047, 75},  // C6
//     {1175, 75},  // D6
//     {1568, 75}   // G6 (Highest note)
// };

const uint8_t MARIO_POWERUP_LENGTH = sizeof(MARIO_POWERUP) / sizeof(Note);

// Ascending Chime: C5-E5-G5-C6 (bright, clean)
const Note ASCENDING_CHIME[] = {
    {523, 100},   // C5
    {0,   30},    // brief silence
    {659, 100},   // E5
    {0,   30},
    {784, 100},   // G5
    {0,   30},
    {1047, 150},  // C6
};
const uint8_t ASCENDING_CHIME_LENGTH = sizeof(ASCENDING_CHIME) / sizeof(Note);

// Two-Tone Beep: A5-C6 (simple, professional)
const Note TWO_TONE_BEEP[] = {
    {880, 120},   // A5
    {0,   40},    // brief silence
    {1047, 160},  // C6
};
const uint8_t TWO_TONE_BEEP_LENGTH = sizeof(TWO_TONE_BEEP) / sizeof(Note);

// Startup melody helpers
const Note* getStartupMelody(uint8_t index) {
    switch (index) {
        case 1: return MARIO_POWERUP;
        case 2: return ASCENDING_CHIME;
        case 3: return TWO_TONE_BEEP;
        default: return nullptr;  // 0 = Silent
    }
}

uint8_t getStartupMelodyLength(uint8_t index) {
    switch (index) {
        case 1: return MARIO_POWERUP_LENGTH;
        case 2: return ASCENDING_CHIME_LENGTH;
        case 3: return TWO_TONE_BEEP_LENGTH;
        default: return 0;
    }
}

const char* getStartupMelodyName(uint8_t index) {
    switch (index) {
        case 0: return "Silent";
        case 1: return "Mario";
        case 2: return "Chime";
        case 3: return "Two-Tone";
        default: return "Unknown";
    }
}

// Critical Warning Alarm: Beep-Beep-Pause (Repeatable)
// High pitch (3000Hz) cuts through noise better than low pitch
const Note CRITICAL_WARNING_ALARM[] = {
    {3000, 80},   // High Beep
    {0,    80},   // Silence
    {3000, 80},   // High Beep
    {0,    500}   // Longer Silence before repeating
};

const uint8_t CRITICAL_WARNING_ALARM_LENGTH = sizeof(CRITICAL_WARNING_ALARM) / sizeof(Note);

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