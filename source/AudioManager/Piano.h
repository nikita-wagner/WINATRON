// Create Virtual Piano Set


#include "Audio.h"
#include <Windows.h>
#include <math.h>
#include <stdio.h>
#include "..\InputManager\Keyboard.h"
#include "..\ConsoleManager\Console.h"
#include "..\ClockManager\Clock.h"

#pragma once

static int ClockPiano = 0; 

// Returns frequency in Hz for a given MIDI key number (can be any integer).
// Standard: MIDI note 69 = A4 = 440 Hz. Frequency formula:
// f(n) = 440 * 2^{(n-69)/12}
static inline float midi(INT PianoKeyIndex) {
    double n = (double)PianoKeyIndex;
    double freq = 440.0 * pow(2.0, (n - 69.0) / 12.0);
    return (float)freq;
}

static int init_Piano(){
    ClockPiano = clock_create(1,"PianoPLay");
    return ClockPiano;
}


static void PlayPiano(){
        for (int vk = 0x08; vk <= 0xFF; vk++) {
            if (input_key_held(vk)) {
                // Try to print a readable name for common keys
                if (vk >= 0x41 && vk <= 0x5A) play_tone_at_buffer_pos_by_duration(vk, midi(vk), 0.01, 0.0, 0.1, 0);        // A-Z
                    if(clock_sync(ClockPiano)){
                        char str[32]; // Ensure the buffer is large enough
                        snprintf(str, sizeof(str), "%f", midi(vk));
                        con_println_color(COL_BLUE, str);
                    }
            }
        }
}