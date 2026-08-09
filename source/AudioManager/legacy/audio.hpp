#if !defined(AUDIO_HPP)
#define AUDIO_HPP

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SAMPLE_RATE 44100
#define AMPLITUDE 150000
#define NUM_BUFFERS 3
#define BUFFER_SIZE 2205
#define PI 3.14159265358979323846
#define MAX_TONE_SOUNDS 100
#define MAX_WAV_SOUNDS 100
#define MAX_WAV_CACHE 32
#define FADE_SAMPLES 4410*16 // 50 ms fade duration
#define REVERB_BUF_LEN BUFFER_SIZE * 4 // 4 buffers worth of reverb delay
#define ANGLE_TO_RADIANS(angle) ((angle) * PI / 180.0)
#define RADIANS_TO_ANGLE(radians) ((radians) * 180.0 / PI)

#pragma comment(lib, "winmm.lib")

typedef struct {
    double frequency;
    double base_frequency;  // ← ADD: set once at play time, never modified
    double phase;
    float amplitude;
    float angle;
    float left_amp;
    float right_amp;
    bool active;
    int fade_state;
    int fade_counter;
    int fade_duration;
    int timer_samples;
    int timer_counter;
    int delay_samples;
    int delay_counter;
    bool is_timed_after_delay;
    double delayed_duration_seconds;
    int sound_index;
    float reverb_amount;
    float reverb_decay;
    // FIX: reverb_buffer moved to static side arrays in sound.cpp.
    //      This reduces sizeof(Tone) from ~17.7 KB to ~80 bytes,
    //      cutting the lock-held copy in audio_mixer by ~220x per slot.
    bool paused;
} Tone;

typedef struct {
    short* data;
    int sample_count;
    int channels;
    int sample_rate;
    char filename[256];
    bool loaded;
} WAV;

typedef struct {
    int sample_rate;
    int total_samples;
} WAVInfo;

typedef struct {
    WAV* wav_data;
    int current_position;
    float fractional_position;
    float amplitude;
    float angle;
    float left_amp;
    float right_amp;
    bool active;
    bool repeat;
    int fade_state;
    int fade_counter;
    int fade_duration;
    int timer_samples;
    int timer_counter;
    int delay_samples;
    int delay_counter;
    bool is_timed_after_delay;
    double delayed_duration_seconds;
    int sound_index;
    float reverb_amount;
    float reverb_decay;
    int loop_start_sample;
    float pitch;        // ← ADD: 1.0 = normal, 2.0 = octave up, 0.5 = octave down
    bool paused;
} WavSound;

typedef struct {
    HWAVEOUT hWaveOut;
    WAVEHDR waveHeaders[3];
    short audioBuffers[3][BUFFER_SIZE * 2];
    int currentBuffer;
    Tone tone[MAX_TONE_SOUNDS];
    WavSound wav[MAX_WAV_SOUNDS];
    WAV wav_cache[MAX_WAV_CACHE];
    int wav_cache_count;
    bool initialized;
    bool running;
    HANDLE audioThread;
    CRITICAL_SECTION toneLock;
    CRITICAL_SECTION wavLock;
} AudioSystem;

extern AudioSystem g_audioSystem;

// Tone Manager Class
class AudioManager {
public:
    // Audio System Methods
    bool audio_init(void);
    void audio_shutdown(void);

    // Tone Generation Methods
    int play_tone_by_duration(int id, double frequency, float amplitude, double phase, double duration_seconds);
    int play_static_tone(int id, double frequency, float amplitude, double phase);
    int play_delayed_tone_by_duration(int id, double frequency, float amplitude, double phase, double duration_seconds, double start_delay_seconds);
    int play_delayed_static_tone(int id, double frequency, float amplitude, double phase, double start_delay_seconds);
    
    // Tone Generation Methods (buffer-synchronized - align to next buffer boundary)
    int play_tone_by_duration_sync(int id, double frequency, float amplitude, double phase, double duration_seconds);
    int play_static_tone_sync(int id, double frequency, float amplitude, double phase);
    
    void stop_tone(int id);
    void stop_all_tones(void);
    void pause_tone(int id);
    void resume_tone(int id);
    void set_amp_tone(int id, float amplitude);

    // Tone Effects Methods
    void angle_of_tone(int id, float angle);
    void reverb_tone(int id, float amount, float decay);
    bool check_active_tone(int id);





    
    // WAV File Methods
    int play_WAV_by_duration(int id, const char* filename, float amplitude, double duration_seconds);
    int play_repeating_WAV(int id, const char* filename, float amplitude);
    int play_delayed_WAV_by_duration(int id, const char* filename, float amplitude, double duration_seconds, double start_delay_seconds);
    int play_delayed_repeating_WAV(int id, const char* filename, float amplitude, double start_delay_seconds);
    int play_repeating_delayed_WAV_by_duration(int id, const char* filename, float amplitude, double play_duration_seconds, double start_delay_seconds);
    int play_specific_part_WAV(int id, const char* filename, float amplitude, int start_sample, int end_sample);
    int play_repeating_specific_part_WAV(int id, const char* filename, float amplitude, int start_sample, int end_sample);
    void stop_WAV(int id);
    void stop_all_WAVs(void);
    void pause_WAV(int id);
    void resume_WAV(int id);
    void set_amp_WAV(int id, float amplitude);
    void set_fade_duration_WAV(int id, int fade_samples);
    void set_pitch_WAV(int id, float pitch);
    void set_pitch_tone(int id, float pitch);
    bool check_active_WAV(int id);

    // WAV File Management Methods
    bool load_WAV(const char* filename);
    void unload_WAV(const char* filename);
    void unload_all_WAVs(void);
    WAVInfo get_WAV_info(const char* filename);
};

#endif // SOUND_HPP
