#pragma once

#include <stdint.h>
#include <cstdint>
#include "pico/stdlib.h"

// Note structure for melodies
struct Note {
    uint16_t frequency;  // Frequency in Hz (0 = rest/silence)
    uint16_t duration;   // Duration in milliseconds
};

// Startup melodies
extern const Note MARIO_POWERUP[];
extern const uint8_t MARIO_POWERUP_LENGTH;

extern const Note ASCENDING_CHIME[];
extern const uint8_t ASCENDING_CHIME_LENGTH;

extern const Note TWO_TONE_BEEP[];
extern const uint8_t TWO_TONE_BEEP_LENGTH;

extern const Note CRITICAL_WARNING_ALARM[];
extern const uint8_t CRITICAL_WARNING_ALARM_LENGTH;

// Startup melody helpers (index: 0=Silent, 1=Mario, 2=Chime, 3=TwoTone)
const Note* getStartupMelody(uint8_t index);
uint8_t getStartupMelodyLength(uint8_t index);
const char* getStartupMelodyName(uint8_t index);

class Buzzer {
private:
    uint     _pin;
    uint     _slice_num;
    uint     _channel;
    uint32_t _current_wrap;

    alarm_id_t _alarm_id; 

    // Static wrapper to allow C-style timer API to call C++ class method
    static int64_t stopToneCallback(alarm_id_t id, void *user_data);

    // Melody playback state
    const Note* _melody;
    uint8_t _melody_length;
    uint8_t _melody_index;
    bool _playing_melody;

    // Static callback for melody progression
    static int64_t playNextNoteCallback(alarm_id_t id, void *user_data);

public:
    /** * Constructor 
     * @param pin The GPIO pin number
     */
    Buzzer(uint pin);

    /** Initialize the buzzer hardware */
    void init();

    /** * Set the frequency of the buzzer in Hz 
     * @return true if frequency was within valid range
     */
    bool setFrequency(uint32_t frequency);

    /** * Set duty cycle using a normalized 16-bit value.
     * 0 = 0%, 32768 = 50%, 65535 = 100%
     */
    void setDutyCycle(uint16_t duty_cycle);
    
    /** Stop the buzzer (set duty to 0) */
    void stop();

    /** * Play a specific frequency for a duration (Non-blocking).
     * @param frequency Hz (e.g., 440)
     * @param duration_ms Duration in milliseconds (e.g., 500)
     */
    void playTone(uint32_t frequency, uint32_t duration_ms);

    /** * Play a melody (Non-blocking).
     * @param melody Pointer to array of Note structures
     * @param length Number of notes in the melody
     */
    void playMelody(const Note* melody, uint8_t length);

    /** Stop any currently playing melody */
    void stopMelody();

    /** Check if a melody is currently playing */
    bool isPlayingMelody() const { return _playing_melody; }
};