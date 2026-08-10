// ═══════════════════════════════════════════════════════════════════════════
//  Audio.h — single-header WinMM audio engine (tones + WAV playback)
//
//  Sound ID convention:
//    Tones : 0 .. 99            (returned by play_tone / play_*_tone)
//    WAVs  : 100 .. 199         (returned by play_*_WAV)
//    Every WAV function also accepts the bare 0..99 slot for compatibility.
//    angle_of_tone / reverb_tone dispatch on the ID: 0..99 = tone, 100+ = WAV.
//
//  Remaster notes (behavior fixes vs. the original):
//    • REVERB_BUF_LEN macro was unparenthesized, so "% REVERB_BUF_LEN"
//      expanded to "(x % 2205) * 4" — the reverb read/write index jumped
//      chaotically instead of stepping through the delay line.
//    • The mixer used to snapshot sounds, mix ~50 ms, then copy the whole
//      struct back — clobbering any stop/pause/amp/pitch call made in that
//      window (stops would randomly "not take"). Mixing now happens in place
//      under the lock; the structs are small so the hold time is tiny.
//    • A non-repeating WAV reaching end-of-file never deactivated its slot:
//      it stayed "active" forever, leaking slots until playback died.
//    • WavSound.pitch was never initialized for a fresh slot (0.0), freezing
//      the first playback of every slot at sample 0.
//    • base_frequency was only set by play_static_tone, so set_pitch_tone
//      silenced tones started with play_tone / play_tone_by_duration.
//    • Surround: the front/back factor was inverted — sounds BEHIND the
//      listener were 3.3× louder than in front. Front is now full volume,
//      rear is attenuated. AMPLITUDE retuned 150000 → 50000 so the corrected
//      law keeps the same front loudness with no clipping at any angle.
//    • stop_* on a delayed or paused sound now deactivates it immediately
//      (it used to start fading in from silence, or stay stuck forever).
//    • stop_* / timer expiry no longer reset fade_duration, so
//      set_fade_duration_* actually sticks. Duration is clamped ≥ 1 sample
//      (0 produced a NaN envelope).
//    • unload_WAV no longer compacts the cache array — compaction shifted
//      WAV structs that live WavSounds still pointed into (dangling reads).
//    • Specific-part playback used an output-sample timer to find the
//      segment end, which drifted for non-44.1 kHz or pitched files; it now
//      tracks the real end frame (loop_end_sample).
//    • WAV resampling uses linear interpolation (was nearest-neighbor).
//    • Delayed starts are sample-accurate (were quantized to 50 ms buffers).
//    • FADE_SAMPLES was 4410*16 ≈ 1.6 s despite its "50 ms" comment; it is
//      now genuinely 50 ms. Use set_fade_duration_* for longer fades.
//    • Missing <math.h>/<stdlib.h> includes, CreateThread-failure path left
//      the system half-initialized, WAVE_FORMAT_EXTENSIBLE files were
//      rejected, odd-sized RIFF chunks broke the parser, >2-channel files
//      played garbage (now rejected at load).
// ═══════════════════════════════════════════════════════════════════════════

#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <immintrin.h>
#include <intrin.h>

#ifndef true
#define true 1
#endif
#ifndef false
#define false 0
#endif

#define SAMPLE_RATE     44100
#define AMPLITUDE       50000            // tone full-scale: gain 1.0 peaks at ~32500 after panning (no clipping)
#define NUM_BUFFERS     3
#define BUFFER_SIZE     2205             // 50 ms per buffer → ~150 ms output latency
#define PI              3.14159265358979323846
#define MAX_TONE_SOUNDS 100
#define MAX_WAV_SOUNDS  100
#define MAX_WAV_CACHE   32
#define FADE_SAMPLES    2205             // 50 ms default fade duration
#define REVERB_BUF_LEN  8820             // 200 ms reverb delay line
#define ANGLE_TO_RADIANS(angle)   ((angle) * PI / 180.0)
#define RADIANS_TO_ANGLE(radians) ((radians) * 180.0 / PI)

#pragma comment(lib, "winmm.lib")

// ─── Fade / lifecycle states ─────────────────────────────────────────────────

typedef enum {
    FADE_IN      = 0,   // ramping up after start
    FADE_SUSTAIN = 1,   // steady state (fade-in finished, or instant attack)
    FADE_OUT     = 2,   // ramping down; slot is freed when envelope hits zero
    FADE_TIMED   = 3,   // instant attack; fade out when the sample timer expires
    FADE_DELAY   = 4    // waiting for a delayed start
} FadeState;

// ─── Types ───────────────────────────────────────────────────────────────────

typedef struct {
    double frequency;
    double base_frequency;      // set at play time; set_pitch_tone scales from this
    double phase;
    float  amplitude;
    float  angle;
    float  left_amp;
    float  right_amp;
    BOOL   active;
    int    fade_state;
    int    fade_counter;
    int    fade_duration;
    int    timer_samples;
    int    timer_counter;
    int    delay_samples;
    int    delay_counter;
    BOOL   is_timed_after_delay;
    double delayed_duration_seconds;
    int    sound_index;
    float  reverb_amount;
    float  reverb_decay;
    // reverb delay lines live in static side arrays (see below) so this
    // struct stays ~100 bytes instead of ~17.7 KB
    BOOL   paused;
} Tone;

typedef struct {
    short* data;
    int    sample_count;        // frames (per channel)
    int    channels;
    int    sample_rate;
    char   filename[256];
    BOOL   loaded;
} WAV;

typedef struct {
    int sample_rate;
    int total_samples;          // frames
} WAVInfo;

typedef struct {
    WAV*   wav_data;
    int    current_position;    // frame index
    float  fractional_position; // sub-frame position for resampling
    float  amplitude;
    float  angle;
    float  left_amp;
    float  right_amp;
    BOOL   active;
    BOOL   repeat;
    int    fade_state;
    int    fade_counter;
    int    fade_duration;
    int    timer_samples;
    int    timer_counter;
    int    delay_samples;
    int    delay_counter;
    BOOL   is_timed_after_delay;
    double delayed_duration_seconds;
    int    sound_index;
    float  reverb_amount;
    float  reverb_decay;
    int    loop_start_sample;   // frame the repeat wraps back to
    int    loop_end_sample;     // frame the segment ends at (0 = end of file)
    float  pitch;               // 1.0 = normal, 2.0 = octave up, 0.5 = octave down
    BOOL   paused;
} WavSound;

typedef struct {
    HWAVEOUT         hWaveOut;
    WAVEHDR          waveHeaders[NUM_BUFFERS];
    short            audioBuffers[NUM_BUFFERS][BUFFER_SIZE * 2];
    int              currentBuffer;
    Tone             tone[MAX_TONE_SOUNDS];
    WavSound         wav[MAX_WAV_SOUNDS];
    WAV              wav_cache[MAX_WAV_CACHE];
    int              wav_cache_count;
    BOOL             initialized;
    volatile BOOL    running;           // read by the audio thread without a lock
    HANDLE           audioThread;
    CRITICAL_SECTION toneLock;
    CRITICAL_SECTION wavLock;
} AudioSystem;

static AudioSystem g_audioSystem = {0};

// ─── Reverb side arrays ──────────────────────────────────────────────────────
// Kept out of Tone/WavSound so the structs stay small. Only the audio thread
// touches the buffer contents; play-time resets happen under the same lock
// the mixer holds, so there is no unlocked concurrent access.

static short reverb_bufs_tone[MAX_TONE_SOUNDS][REVERB_BUF_LEN];
static int   reverb_idx_tone [MAX_TONE_SOUNDS];

static short reverb_bufs_wav [MAX_WAV_SOUNDS][REVERB_BUF_LEN];
static int   reverb_idx_wav  [MAX_WAV_SOUNDS];

// ─── Sine table ──────────────────────────────────────────────────────────────

#define SINE_TABLE_SIZE 1024
static float sine_table[SINE_TABLE_SIZE];
static BOOL  sine_table_initialized = false;

// Precomputed scale: fast_sin is one multiply + mask, no divide/floor.
static const double SINE_SCALE = (double)SINE_TABLE_SIZE / (2.0 * PI);

static void init_sine_table(void) {
    if (sine_table_initialized) return;
    for (int i = 0; i < SINE_TABLE_SIZE; i++)
        sine_table[i] = (float)sin(2.0 * PI * i / SINE_TABLE_SIZE);
    sine_table_initialized = true;
}

// Callers keep phase in [0, 2π); the mask makes stray values safe anyway.
static inline float fast_sin(double phase) {
    return sine_table[(int)(phase * SINE_SCALE) & (SINE_TABLE_SIZE - 1)];
}

// ─── Stereo panning ──────────────────────────────────────────────────────────
// angle 0 = front, 90 = right, 180 = behind, 270 = left.
// Left/right from sin(angle); front/back loudness from cos(angle):
// full volume in front, attenuated to 0.3 behind the listener.

static inline void calculate_stereo_amplitudes(float angle, float* left_amp, float* right_amp) {
    angle = fmodf(angle, 360.0f);
    if (angle < 0) angle += 360.0f;

    float rad          = (float)ANGLE_TO_RADIANS(angle);
    float lr_component = sinf(rad);
    float fb_component = cosf(rad);

    float distance_factor = 0.65f + fb_component * 0.35f;

    *right_amp = (0.5f + lr_component * 0.5f) * distance_factor;
    *left_amp  = (0.5f - lr_component * 0.5f) * distance_factor;

    if (*left_amp  < 0) *left_amp  = 0;
    if (*right_amp < 0) *right_amp = 0;
    if (*left_amp  > 1) *left_amp  = 1;
    if (*right_amp > 1) *right_amp = 1;
}

// ─── Shared helpers ──────────────────────────────────────────────────────────

static inline float tick_fade_in(int* counter, int duration, int* state) {
    float env = (float)(*counter) / (float)duration;
    if (env > 1.0f) env = 1.0f;
    (*counter)++;
    if (*counter >= duration) *state = FADE_SUSTAIN;
    return env;
}

static inline float tick_fade_out(int* counter, int duration, BOOL* active) {
    float env = 1.0f - (float)(*counter) / (float)duration;
    (*counter)++;
    if (env <= 0.0f) { *active = false; return -1.0f; }
    return env;
}

static inline float apply_reverb(float sample, short* buf, int* idx,
                                 float amount, float decay) {
    float wet = sample + buf[*idx] * amount;
    float fb  = wet * decay;
    // clamp the feedback before it lands in the 16-bit delay line —
    // integer wraparound here produced harsh crackle on loud sources
    if (fb >  32767.0f) fb =  32767.0f;
    if (fb < -32768.0f) fb = -32768.0f;
    buf[*idx] = (short)fb;
    *idx = (*idx + 1) % REVERB_BUF_LEN;
    return wet / (1.0f + amount * 0.5f);
}

static inline void mix_stereo(short* buffer, int i,
                              float sample, float left_amp, float right_amp) {
    int L = buffer[i * 2]     + (int)(sample * left_amp);
    int R = buffer[i * 2 + 1] + (int)(sample * right_amp);
    buffer[i * 2]     = (short)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
    buffer[i * 2 + 1] = (short)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
}

// ─── Sound initializers ──────────────────────────────────────────────────────
// Both take the slot index so they can reset that slot's reverb delay line.

static void init_sound_common(Tone* sound, int slot,
                              double frequency, float amplitude, double phase) {
    phase = fmod(phase, 2.0 * PI);
    if (phase < 0.0) phase += 2.0 * PI;

    sound->frequency                = frequency;
    sound->base_frequency           = frequency;
    sound->phase                    = phase;
    sound->amplitude                = amplitude;
    sound->angle                    = 0.0f;
    sound->left_amp                 = 1.0f;
    sound->right_amp                = 1.0f;
    sound->active                   = true;
    sound->fade_state               = FADE_IN;
    sound->fade_counter             = 0;
    sound->fade_duration            = FADE_SAMPLES;
    sound->timer_samples            = 0;
    sound->timer_counter            = 0;
    sound->delay_samples            = 0;
    sound->delay_counter            = 0;
    sound->is_timed_after_delay     = false;
    sound->delayed_duration_seconds = 0.0;
    sound->reverb_amount            = 0.0f;
    sound->reverb_decay             = 0.5f;
    sound->paused                   = false;

    memset(reverb_bufs_tone[slot], 0, REVERB_BUF_LEN * sizeof(short));
    reverb_idx_tone[slot] = 0;
}

static void init_wav_sound_common(WavSound* sound, int slot,
                                  WAV* wav_data, float amplitude) {
    sound->wav_data                 = wav_data;
    sound->current_position         = 0;
    sound->fractional_position      = 0.0f;
    sound->amplitude                = amplitude;
    sound->angle                    = 0.0f;
    sound->left_amp                 = 1.0f;
    sound->right_amp                = 1.0f;
    sound->active                   = true;
    sound->repeat                   = false;
    sound->fade_state               = FADE_IN;
    sound->fade_counter             = 0;
    sound->fade_duration            = FADE_SAMPLES;
    sound->timer_samples            = 0;
    sound->timer_counter            = 0;
    sound->delay_samples            = 0;
    sound->delay_counter            = 0;
    sound->is_timed_after_delay     = false;
    sound->delayed_duration_seconds = 0.0;
    sound->reverb_amount            = 0.0f;
    sound->reverb_decay             = 0.5f;
    sound->loop_start_sample        = 0;
    sound->loop_end_sample          = 0;
    sound->pitch                    = 1.0f;
    sound->paused                   = false;

    memset(reverb_bufs_wav[slot], 0, REVERB_BUF_LEN * sizeof(short));
    reverb_idx_wav[slot] = 0;
}

// ─── Mixer ───────────────────────────────────────────────────────────────────
// Sounds are mixed IN PLACE while holding their lock. The structs are small
// and the per-buffer work is a few hundred microseconds even fully loaded, so
// API calls block only briefly — and in exchange no stop/pause/amp/pitch
// update can ever be lost to a snapshot copy-back.

// Handles the FADE_DELAY state. Returns the buffer index to start mixing at,
// or a negative value if the whole buffer is still inside the delay.
static inline int tick_delay(int* fade_state, int* delay_counter, int delay_samples,
                             BOOL is_timed_after_delay, double delayed_duration_seconds,
                             int* timer_samples, int* timer_counter, int* fade_counter,
                             int buffer_size) {
    if (*delay_counter + buffer_size < delay_samples) {
        *delay_counter += buffer_size;
        return -1;
    }
    int start_i = delay_samples - *delay_counter;   // remaining delay, in samples
    if (start_i < 0)           start_i = 0;
    if (start_i > buffer_size) start_i = buffer_size;
    *delay_counter = delay_samples;

    if (is_timed_after_delay) {
        *fade_state    = FADE_TIMED;
        *timer_samples = (int)(delayed_duration_seconds * SAMPLE_RATE);
        *timer_counter = 0;
    } else {
        *fade_state = FADE_IN;
    }
    *fade_counter = 0;
    return start_i;
}

static void audio_mixer(short* buffer, int buffer_size) {
    memset(buffer, 0, (size_t)buffer_size * 2 * sizeof(short));

    // ── Tone sounds ──────────────────────────────────────────────────────────
    EnterCriticalSection(&g_audioSystem.toneLock);
    for (int v = 0; v < MAX_TONE_SOUNDS; v++) {
        Tone* tone = &g_audioSystem.tone[v];
        if (!tone->active || tone->paused) continue;

        int start_i = 0;
        if (tone->fade_state == FADE_DELAY) {
            start_i = tick_delay(&tone->fade_state, &tone->delay_counter,
                                 tone->delay_samples, tone->is_timed_after_delay,
                                 tone->delayed_duration_seconds,
                                 &tone->timer_samples, &tone->timer_counter,
                                 &tone->fade_counter, buffer_size);
            if (start_i < 0) continue;
        }

        double phase_inc = 2.0 * PI * tone->frequency / SAMPLE_RATE;
        calculate_stereo_amplitudes(tone->angle, &tone->left_amp, &tone->right_amp);

        for (int i = start_i; i < buffer_size; i++) {
            float sample = fast_sin(tone->phase) * tone->amplitude * AMPLITUDE;
            tone->phase += phase_inc;
            if (tone->phase >= 2.0 * PI) tone->phase -= 2.0 * PI;

            if (tone->reverb_amount > 0.0f)
                sample = apply_reverb(sample,
                                      reverb_bufs_tone[v], &reverb_idx_tone[v],
                                      tone->reverb_amount, tone->reverb_decay);

            float env = 1.0f;
            if (tone->fade_state == FADE_IN) {
                env = tick_fade_in(&tone->fade_counter, tone->fade_duration, &tone->fade_state);
            } else if (tone->fade_state == FADE_OUT) {
                env = tick_fade_out(&tone->fade_counter, tone->fade_duration, &tone->active);
                if (env < 0.0f) break;
            } else if (tone->fade_state == FADE_TIMED) {
                tone->timer_counter++;
                if (tone->timer_counter >= tone->timer_samples) {
                    tone->fade_state   = FADE_OUT;
                    tone->fade_counter = 0;
                }
            }

            mix_stereo(buffer, i, sample * env, tone->left_amp, tone->right_amp);
        }
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);

    // ── WAV sounds ───────────────────────────────────────────────────────────
    EnterCriticalSection(&g_audioSystem.wavLock);
    for (int v = 0; v < MAX_WAV_SOUNDS; v++) {
        WavSound* s  = &g_audioSystem.wav[v];
        if (!s->active) continue;

        WAV* wd = s->wav_data;
        if (!wd || !wd->loaded || !wd->data) { s->active = false; continue; }
        if (s->paused) continue;

        int start_i = 0;
        if (s->fade_state == FADE_DELAY) {
            start_i = tick_delay(&s->fade_state, &s->delay_counter,
                                 s->delay_samples, s->is_timed_after_delay,
                                 s->delayed_duration_seconds,
                                 &s->timer_samples, &s->timer_counter,
                                 &s->fade_counter, buffer_size);
            if (start_i < 0) continue;
        }

        float rate_ratio = ((float)wd->sample_rate / (float)SAMPLE_RATE) * s->pitch;
        calculate_stereo_amplitudes(s->angle, &s->left_amp, &s->right_amp);

        int end_frame = (s->loop_end_sample > 0 && s->loop_end_sample <= wd->sample_count)
                            ? s->loop_end_sample : wd->sample_count;

        for (int i = start_i; i < buffer_size; i++) {
            if (s->current_position >= end_frame) {
                if (s->repeat) {
                    s->current_position = s->loop_start_sample;
                } else if (end_frame < wd->sample_count) {
                    // segment finished: fade out over the audio that follows it
                    if (s->fade_state != FADE_OUT) {
                        s->fade_state   = FADE_OUT;
                        s->fade_counter = 0;
                    }
                    s->loop_end_sample = 0;
                    end_frame          = wd->sample_count;
                    if (s->current_position >= end_frame) { s->active = false; break; }
                } else {
                    s->active = false;
                    break;
                }
            }

            // linear interpolation between this frame and the next
            int p0 = s->current_position;
            int p1 = p0 + 1;
            if (p1 >= end_frame) p1 = s->repeat ? s->loop_start_sample : p0;

            float frac = s->fractional_position;
            float wav_sample;
            if (wd->channels == 1) {
                float a = (float)wd->data[p0];
                float b = (float)wd->data[p1];
                wav_sample = a + frac * (b - a);
            } else {
                float a = ((float)wd->data[p0 * 2] + (float)wd->data[p0 * 2 + 1]) * 0.5f;
                float b = ((float)wd->data[p1 * 2] + (float)wd->data[p1 * 2 + 1]) * 0.5f;
                wav_sample = a + frac * (b - a);
            }
            wav_sample *= s->amplitude;

            if (s->reverb_amount > 0.0f)
                wav_sample = apply_reverb(wav_sample,
                                          reverb_bufs_wav[v], &reverb_idx_wav[v],
                                          s->reverb_amount, s->reverb_decay);

            float env = 1.0f;
            if (s->fade_state == FADE_IN) {
                env = tick_fade_in(&s->fade_counter, s->fade_duration, &s->fade_state);
            } else if (s->fade_state == FADE_OUT) {
                env = tick_fade_out(&s->fade_counter, s->fade_duration, &s->active);
                if (env < 0.0f) break;
            } else if (s->fade_state == FADE_TIMED) {
                s->timer_counter++;
                if (s->timer_counter >= s->timer_samples) {
                    if (s->repeat) {
                        // retrigger: restart the loop every timer_samples
                        s->current_position    = s->loop_start_sample;
                        s->fractional_position = 0.0f;
                        s->timer_counter       = 0;
                    } else {
                        s->fade_state   = FADE_OUT;
                        s->fade_counter = 0;
                    }
                }
            }

            mix_stereo(buffer, i, wav_sample * env, s->left_amp, s->right_amp);

            s->fractional_position += rate_ratio;
            int advance = (int)s->fractional_position;
            if (advance > 0) {
                s->current_position    += advance;
                s->fractional_position -= (float)advance;
            }
        }
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

// ─── Audio thread ────────────────────────────────────────────────────────────

static DWORD WINAPI audio_thread_proc(LPVOID lpParam) {
    (void)lpParam;
    while (g_audioSystem.running) {
        for (int i = 0; i < NUM_BUFFERS; i++) {
            WAVEHDR* header = &g_audioSystem.waveHeaders[i];
            if ((header->dwFlags & WHDR_DONE) || !(header->dwFlags & WHDR_PREPARED)) {
                audio_mixer(g_audioSystem.audioBuffers[i], BUFFER_SIZE);
                if (header->dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(g_audioSystem.hWaveOut, header, sizeof(WAVEHDR));

                header->dwFlags = 0;
                if (waveOutPrepareHeader(g_audioSystem.hWaveOut, header, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                    waveOutWrite(g_audioSystem.hWaveOut, header, sizeof(WAVEHDR));
            }
        }
        Sleep(3);
    }
    return 0;
}

// ─── Init / Shutdown ─────────────────────────────────────────────────────────

// Forward declarations (defined in the WAV section below)
void stop_all_tones(void);
void stop_all_WAVs(void);
void unload_all_WAVs(void);

BOOL audio_init(void) {
    if (g_audioSystem.initialized)
        return true;

    memset(&g_audioSystem, 0, sizeof(AudioSystem));

    InitializeCriticalSection(&g_audioSystem.toneLock);
    InitializeCriticalSection(&g_audioSystem.wavLock);

    WAVEFORMATEX waveFormat;
    waveFormat.wFormatTag      = WAVE_FORMAT_PCM;
    waveFormat.nChannels       = 2;
    waveFormat.nSamplesPerSec  = SAMPLE_RATE;
    waveFormat.nAvgBytesPerSec = SAMPLE_RATE * 2 * sizeof(short);
    waveFormat.nBlockAlign     = 4;
    waveFormat.wBitsPerSample  = 16;
    waveFormat.cbSize          = 0;

    MMRESULT result = waveOutOpen(&g_audioSystem.hWaveOut, WAVE_MAPPER, &waveFormat, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        DeleteCriticalSection(&g_audioSystem.toneLock);
        DeleteCriticalSection(&g_audioSystem.wavLock);
        return false;
    }

    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_audioSystem.waveHeaders[i].lpData         = (LPSTR)g_audioSystem.audioBuffers[i];
        g_audioSystem.waveHeaders[i].dwBufferLength = BUFFER_SIZE * 2 * sizeof(short);
        g_audioSystem.waveHeaders[i].dwFlags        = 0;
        g_audioSystem.waveHeaders[i].dwLoops        = 0;
    }

    // memset already zeroed every field — only set non-zero defaults here
    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        g_audioSystem.tone[i].left_amp      = 1.0f;
        g_audioSystem.tone[i].right_amp     = 1.0f;
        g_audioSystem.tone[i].fade_duration = FADE_SAMPLES;
        g_audioSystem.tone[i].reverb_decay  = 0.5f;
        g_audioSystem.tone[i].sound_index   = i;
    }

    for (int i = 0; i < MAX_WAV_SOUNDS; i++) {
        g_audioSystem.wav[i].left_amp      = 1.0f;
        g_audioSystem.wav[i].right_amp     = 1.0f;
        g_audioSystem.wav[i].fade_duration = FADE_SAMPLES;
        g_audioSystem.wav[i].reverb_decay  = 0.5f;
        g_audioSystem.wav[i].pitch         = 1.0f;
        g_audioSystem.wav[i].sound_index   = i;
    }

    memset(reverb_bufs_tone, 0, sizeof(reverb_bufs_tone));
    memset(reverb_idx_tone,  0, sizeof(reverb_idx_tone));
    memset(reverb_bufs_wav,  0, sizeof(reverb_bufs_wav));
    memset(reverb_idx_wav,   0, sizeof(reverb_idx_wav));

    init_sine_table();

    g_audioSystem.initialized = true;
    g_audioSystem.running     = true;

    g_audioSystem.audioThread = CreateThread(NULL, 0, audio_thread_proc, NULL, 0, NULL);
    if (g_audioSystem.audioThread == NULL) {
        g_audioSystem.initialized = false;
        g_audioSystem.running     = false;
        waveOutClose(g_audioSystem.hWaveOut);
        DeleteCriticalSection(&g_audioSystem.toneLock);
        DeleteCriticalSection(&g_audioSystem.wavLock);
        return false;
    }
    // keep buffer refills punctual even when the main thread is busy
    SetThreadPriority(g_audioSystem.audioThread, THREAD_PRIORITY_TIME_CRITICAL);

    return true;
}

void audio_shutdown(void) {
    if (!g_audioSystem.initialized)
        return;

    g_audioSystem.running = false;
    if (g_audioSystem.audioThread != NULL) {
        WaitForSingleObject(g_audioSystem.audioThread, 2000);
        CloseHandle(g_audioSystem.audioThread);
        g_audioSystem.audioThread = NULL;
    }

    waveOutReset(g_audioSystem.hWaveOut);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_audioSystem.waveHeaders[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_audioSystem.hWaveOut, &g_audioSystem.waveHeaders[i], sizeof(WAVEHDR));
    }

    waveOutClose(g_audioSystem.hWaveOut);

    unload_all_WAVs();

    DeleteCriticalSection(&g_audioSystem.toneLock);
    DeleteCriticalSection(&g_audioSystem.wavLock);

    g_audioSystem.initialized = false;
}

// ─── Tone API ────────────────────────────────────────────────────────────────

// Must be called with toneLock held. Explicit id claims that slot (restarting
// whatever occupied it); any other id auto-picks the first free slot.
static int acquire_tone_slot(int id) {
    if (id >= 0 && id < MAX_TONE_SOUNDS) return id;
    for (int i = 0; i < MAX_TONE_SOUNDS; i++)
        if (!g_audioSystem.tone[i].active) return i;
    return -1;
}

int play_tone(double frequency, float gain) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);
    int slot = acquire_tone_slot(-1);
    if (slot >= 0)
        init_sound_common(&g_audioSystem.tone[slot], slot, frequency, gain, 0.0);
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return slot;
}

int play_tone_by_duration(int id, double frequency, float amplitude, double phase, double duration_seconds) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);
    int slot = acquire_tone_slot(id);
    if (slot >= 0) {
        Tone* sound = &g_audioSystem.tone[slot];
        init_sound_common(sound, slot, frequency, amplitude, phase);
        sound->fade_state    = FADE_TIMED;
        sound->timer_samples = (int)(duration_seconds * SAMPLE_RATE);
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return slot;
}

int play_static_tone(int id, double frequency, float amplitude, double phase) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);
    int slot = acquire_tone_slot(id);
    if (slot >= 0)
        init_sound_common(&g_audioSystem.tone[slot], slot, frequency, amplitude, phase);
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return slot;
}

int play_delayed_tone_by_duration(int id, double frequency, float amplitude, double phase,
                                  double duration_seconds, double start_delay_seconds) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);
    int slot = acquire_tone_slot(id);
    if (slot >= 0) {
        Tone* sound = &g_audioSystem.tone[slot];
        init_sound_common(sound, slot, frequency, amplitude, phase);
        sound->fade_state               = FADE_DELAY;
        sound->delay_samples            = (int)(start_delay_seconds * SAMPLE_RATE);
        sound->is_timed_after_delay     = true;
        sound->delayed_duration_seconds = duration_seconds;
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return slot;
}

int play_delayed_static_tone(int id, double frequency, float amplitude, double phase,
                             double start_delay_seconds) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);
    int slot = acquire_tone_slot(id);
    if (slot >= 0) {
        Tone* sound = &g_audioSystem.tone[slot];
        init_sound_common(sound, slot, frequency, amplitude, phase);
        sound->fade_state           = FADE_DELAY;
        sound->delay_samples        = (int)(start_delay_seconds * SAMPLE_RATE);
        sound->is_timed_after_delay = false;
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return slot;
}

void stop_tone(int sound_id) {
    if (!g_audioSystem.initialized || sound_id < 0 || sound_id >= MAX_TONE_SOUNDS) return;

    EnterCriticalSection(&g_audioSystem.toneLock);
    Tone* sound = &g_audioSystem.tone[sound_id];
    if (sound->active) {
        if (sound->fade_state == FADE_DELAY || sound->paused) {
            sound->active = false;      // silent right now — no fade needed
            sound->paused = false;
        } else if (sound->fade_state != FADE_OUT) {
            sound->fade_state   = FADE_OUT;
            sound->fade_counter = 0;
        }
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void stop_all_tones(void) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.toneLock);
    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        Tone* sound = &g_audioSystem.tone[i];
        if (!sound->active) continue;
        if (sound->fade_state == FADE_DELAY || sound->paused) {
            sound->active = false;
            sound->paused = false;
        } else if (sound->fade_state != FADE_OUT) {
            sound->fade_state   = FADE_OUT;
            sound->fade_counter = 0;
        }
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

// id 0..99 = tone, 100..199 = WAV
void angle_of_tone(int id, float angle) {
    if (!g_audioSystem.initialized) return;

    if (id >= 0 && id < MAX_TONE_SOUNDS) {
        EnterCriticalSection(&g_audioSystem.toneLock);
        Tone* sound = &g_audioSystem.tone[id];
        if (sound->active)
            sound->angle = angle;   // mixer recomputes left/right each buffer
        LeaveCriticalSection(&g_audioSystem.toneLock);

    } else if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) {
        EnterCriticalSection(&g_audioSystem.wavLock);
        WavSound* ws = &g_audioSystem.wav[id - 100];
        if (ws->active)
            ws->angle = angle;
        LeaveCriticalSection(&g_audioSystem.wavLock);
    }
}

// id 0..99 = tone, 100..199 = WAV
void reverb_tone(int id, float amount, float decay) {
    if (!g_audioSystem.initialized) return;

    if (amount < 0.0f) amount = 0.0f;
    if (amount > 1.0f) amount = 1.0f;
    if (decay  < 0.1f) decay  = 0.1f;
    if (decay  > 0.9f) decay  = 0.9f;

    if (id >= 0 && id < MAX_TONE_SOUNDS) {
        EnterCriticalSection(&g_audioSystem.toneLock);
        Tone* sound = &g_audioSystem.tone[id];
        if (sound->active) {
            sound->reverb_amount = amount;
            sound->reverb_decay  = decay;
        }
        LeaveCriticalSection(&g_audioSystem.toneLock);

    } else if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) {
        EnterCriticalSection(&g_audioSystem.wavLock);
        WavSound* ws = &g_audioSystem.wav[id - 100];
        if (ws->active) {
            ws->reverb_amount = amount;
            ws->reverb_decay  = decay;
        }
        LeaveCriticalSection(&g_audioSystem.wavLock);
    }
}

void set_amp_tone(int id, float amplitude) {
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return;

    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;

    EnterCriticalSection(&g_audioSystem.toneLock);
    if (g_audioSystem.tone[id].active)
        g_audioSystem.tone[id].amplitude = amplitude;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void pause_tone(int id) {
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return;
    EnterCriticalSection(&g_audioSystem.toneLock);
    if (g_audioSystem.tone[id].active)
        g_audioSystem.tone[id].paused = true;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void resume_tone(int id) {
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return;
    EnterCriticalSection(&g_audioSystem.toneLock);
    if (g_audioSystem.tone[id].active)
        g_audioSystem.tone[id].paused = false;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void set_pitch_tone(int id, float pitch) {
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return;
    EnterCriticalSection(&g_audioSystem.toneLock);
    if (g_audioSystem.tone[id].active)
        g_audioSystem.tone[id].frequency = g_audioSystem.tone[id].base_frequency * (double)pitch;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

// Relative version: multiplies the CURRENT frequency (compounds per call).
void set_pitch_tone_dec_inc(int id, float pitch) {
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return;
    EnterCriticalSection(&g_audioSystem.toneLock);
    if (g_audioSystem.tone[id].active)
        g_audioSystem.tone[id].frequency *= (double)pitch;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void set_fade_duration_tone(int id, int samples) {
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return;
    if (samples < 1) samples = 1;   // 0 would divide by zero in the envelope

    EnterCriticalSection(&g_audioSystem.toneLock);
    g_audioSystem.tone[id].fade_duration = samples;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

BOOL check_active_tone(int id) {
    // Returns true while a sound is fading out (still audible) — intentional.
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return false;
    EnterCriticalSection(&g_audioSystem.toneLock);
    BOOL playing = g_audioSystem.tone[id].active;
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return playing;
}


// ─────────────────────────────────────────────//
////////////////// WAV Section////////////////////
// ─────────────────────────────────────────────//

// ─── WAV file structures ─────────────────────────────────────────────────────

#pragma pack(push, 1)
typedef struct { char riff[4]; UINT32 chunk_size; char wave[4]; } RiffHeader;
typedef struct { char id[4];   UINT32 size;                      } ChunkHeader;
typedef struct {
    UINT16 format;
    UINT16 channels;
    UINT32 sample_rate;
    UINT32 byte_rate;
    UINT16 block_align;
    UINT16 bits_per_sample;
} FmtChunk;
#pragma pack(pop)

#define WAVE_FMT_PCM        0x0001
#define WAVE_FMT_EXTENSIBLE 0xFFFE  // PCM wrapped in an extensible header

// ─────────────────────────────────────────────────────────────────────────────
// CPUID feature detection (runtime, cached)
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    BOOL sse2;
    BOOL avx2;
    BOOL avx512f;
    BOOL avx512bw;
    BOOL avx512vbmi;
} CpuFeatures;

static inline CpuFeatures detect_cpu_features(void) {
    static CpuFeatures f = {false, false, false, false, false};
    static BOOL cached   = false;
    if (cached) return f;

    int info[4];

    // ── Leaf 1 ───────────────────────────────────────────────────────────────
    __cpuid(info, 1);
    f.sse2 = (info[3] & (1 << 26)) != 0;

    BOOL osxsave = (info[2] & (1 << 27)) != 0;
    BOOL avx_ecx = (info[2] & (1 << 28)) != 0;

    if (!osxsave || !avx_ecx) { cached = true; return f; }

    UINT64 xcr0 = _xgetbv(0);
    BOOL ymm_ok = (xcr0 & 0x06) == 0x06;
    BOOL zmm_ok = ymm_ok && ((xcr0 & 0xE0) == 0xE0);

    // ── Leaf 7, subleaf 0 ────────────────────────────────────────────────────
    __cpuidex(info, 7, 0);
    if (ymm_ok) {
        f.avx2     = (info[1] & (1 << 5))  != 0;
        f.avx512f  = zmm_ok && ((info[1] & (1 << 16)) != 0);
        f.avx512bw = zmm_ok && ((info[1] & (1 << 30)) != 0);
        f.avx512vbmi = f.avx512f && ((info[2] & (1 << 1)) != 0);
    }

    cached = true;
    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
// 8-bit unsigned → 16-bit signed  (subtract 128, scale ×256)
//
// Key optimization: replace  cvtepi8_epi16(x) + slli(x, 8)
//                   with     unpacklo_epi8(zero, x)
//
//   unpacklo_epi8(zero, src) interleaves as: [0, src[0], 0, src[1], ...]
//   In little-endian 16-bit that is: low byte = 0, high byte = src[i]
//   → signed 16-bit value = src[i] << 8  ✓ (sign preserved, e.g. 0x80 → 0x8000)
//
//   This halves the instruction count per loop body vs. the original and
//   has better throughput on all µarchs (vpunpcklbw: 0.5 cy vs vpmovsxbw: 1 cy
//   on Zen 4).
//
// AVX2 is also unrolled to 64 bytes/iteration for better ILP, which makes the
// AVX-512 path unnecessary on Zen 4 (which has 256-bit execution units anyway).
// ─────────────────────────────────────────────────────────────────────────────

static inline void _cvt8to16_scalar(const unsigned char* src, short* dst,
                                    int start, int end) {
    for (int i = start; i < end; i++)
        dst[i] = (short)((src[i] - 128) * 256);
}

static inline void _cvt8to16_sse2(const unsigned char* src, short* dst,
                                  int* i, int n) {
    const __m128i offset = _mm_set1_epi8((char)128);
    const __m128i zero   = _mm_setzero_si128();

    for (; *i <= n - 16; *i += 16) {
        __m128i s8 = _mm_sub_epi8(_mm_loadu_si128((const __m128i*)(src + *i)), offset);

        // Each signed byte goes into the high byte of its 16-bit lane → byte × 256
        _mm_storeu_si128((__m128i*)(dst + *i),     _mm_unpacklo_epi8(zero, s8));
        _mm_storeu_si128((__m128i*)(dst + *i + 8), _mm_unpackhi_epi8(zero, s8));
    }
}

static inline void _cvt8to16_avx2(const unsigned char* src, short* dst,
                                  int* i, int n) {
    const __m256i offset = _mm256_set1_epi8((char)128);
    const __m128i zero   = _mm_setzero_si128();

    // 64-byte unrolled loop — two 256-bit loads in flight simultaneously.
    // Preferred over an AVX-512 path on Zen 4 (256-bit execution units) and
    // competitive with true 512-bit hardware.
    for (; *i <= n - 64; *i += 64) {
        __m256i s0 = _mm256_sub_epi8(
            _mm256_loadu_si256((const __m256i*)(src + *i)),      offset);
        __m256i s1 = _mm256_sub_epi8(
            _mm256_loadu_si256((const __m256i*)(src + *i + 32)), offset);

        __m128i s0_lo = _mm256_castsi256_si128(s0);
        __m128i s0_hi = _mm256_extracti128_si256(s0, 1);
        __m128i s1_lo = _mm256_castsi256_si128(s1);
        __m128i s1_hi = _mm256_extracti128_si256(s1, 1);

        // Build four 256-bit outputs using the unpack trick (no shift needed)
        __m256i d0 = _mm256_set_m128i(_mm_unpackhi_epi8(zero, s0_lo),
                                      _mm_unpacklo_epi8(zero, s0_lo));
        __m256i d1 = _mm256_set_m128i(_mm_unpackhi_epi8(zero, s0_hi),
                                      _mm_unpacklo_epi8(zero, s0_hi));
        __m256i d2 = _mm256_set_m128i(_mm_unpackhi_epi8(zero, s1_lo),
                                      _mm_unpacklo_epi8(zero, s1_lo));
        __m256i d3 = _mm256_set_m128i(_mm_unpackhi_epi8(zero, s1_hi),
                                      _mm_unpacklo_epi8(zero, s1_hi));

        _mm256_storeu_si256((__m256i*)(dst + *i),      d0);
        _mm256_storeu_si256((__m256i*)(dst + *i + 16), d1);
        _mm256_storeu_si256((__m256i*)(dst + *i + 32), d2);
        _mm256_storeu_si256((__m256i*)(dst + *i + 48), d3);
    }

    // Tail: handle a remaining 32-byte block if present
    for (; *i <= n - 32; *i += 32) {
        __m256i s = _mm256_sub_epi8(
            _mm256_loadu_si256((const __m256i*)(src + *i)), offset);
        __m128i lo = _mm256_castsi256_si128(s);
        __m128i hi = _mm256_extracti128_si256(s, 1);

        _mm256_storeu_si256((__m256i*)(dst + *i),
            _mm256_set_m128i(_mm_unpackhi_epi8(zero, lo), _mm_unpacklo_epi8(zero, lo)));
        _mm256_storeu_si256((__m256i*)(dst + *i + 16),
            _mm256_set_m128i(_mm_unpackhi_epi8(zero, hi), _mm_unpacklo_epi8(zero, hi)));
    }
}

static inline void convert_8bit_to_16bit_simd(const unsigned char* src, short* dst,
                                              int sample_count) {
    CpuFeatures cpu = detect_cpu_features();
    int i = 0;

    if      (cpu.avx2) _cvt8to16_avx2(src, dst, &i, sample_count);
    else if (cpu.sse2) _cvt8to16_sse2(src, dst, &i, sample_count);

    _cvt8to16_scalar(src, dst, i, sample_count);
}


// ─────────────────────────────────────────────────────────────────────────────
// 24-bit signed → 16-bit signed  (right-shift 8 with sign extension)
//
// The 24-bit packed format means every sample straddles a different byte
// boundary, so scalar extraction + SIMD pack is the practical optimum.
// ─────────────────────────────────────────────────────────────────────────────

static inline void _cvt24to16_scalar(const unsigned char* src, short* dst,
                                     int start, int end) {
    for (int i = start; i < end; i++) {
        int s = src[i*3] | (src[i*3+1] << 8) | (src[i*3+2] << 16);
        if (s & 0x800000) s |= 0xFF000000;
        dst[i] = (short)(s >> 8);
    }
}

static inline void _cvt24to16_sse2(const unsigned char* src, short* dst,
                                   int* i, int n) {
    for (; *i <= n - 4; *i += 4) {
        const unsigned char* p = src + *i * 3;
        int s0 = p[0]  | (p[1]  << 8) | (p[2]  << 16);
        int s1 = p[3]  | (p[4]  << 8) | (p[5]  << 16);
        int s2 = p[6]  | (p[7]  << 8) | (p[8]  << 16);
        int s3 = p[9]  | (p[10] << 8) | (p[11] << 16);
        if (s0 & 0x800000) s0 |= 0xFF000000;
        if (s1 & 0x800000) s1 |= 0xFF000000;
        if (s2 & 0x800000) s2 |= 0xFF000000;
        if (s3 & 0x800000) s3 |= 0xFF000000;
        __m128i v32 = _mm_set_epi32(s3, s2, s1, s0);
        __m128i v16 = _mm_packs_epi32(_mm_srai_epi32(v32, 8), _mm_setzero_si128());
        _mm_storel_epi64((__m128i*)(dst + *i), v16);
    }
}

static inline void _cvt24to16_avx2(const unsigned char* src, short* dst,
                                   int* i, int n) {
    for (; *i <= n - 8; *i += 8) {
        const unsigned char* p = src + *i * 3;
        int s0 = p[0]  | (p[1]  << 8) | (p[2]  << 16);
        int s1 = p[3]  | (p[4]  << 8) | (p[5]  << 16);
        int s2 = p[6]  | (p[7]  << 8) | (p[8]  << 16);
        int s3 = p[9]  | (p[10] << 8) | (p[11] << 16);
        int s4 = p[12] | (p[13] << 8) | (p[14] << 16);
        int s5 = p[15] | (p[16] << 8) | (p[17] << 16);
        int s6 = p[18] | (p[19] << 8) | (p[20] << 16);
        int s7 = p[21] | (p[22] << 8) | (p[23] << 16);

        s0 = (s0 << 8) >> 8;  s1 = (s1 << 8) >> 8;
        s2 = (s2 << 8) >> 8;  s3 = (s3 << 8) >> 8;
        s4 = (s4 << 8) >> 8;  s5 = (s5 << 8) >> 8;
        s6 = (s6 << 8) >> 8;  s7 = (s7 << 8) >> 8;

        __m128i a32 = _mm_set_epi32(s3, s2, s1, s0);
        __m128i b32 = _mm_set_epi32(s7, s6, s5, s4);
        __m128i a16 = _mm_packs_epi32(_mm_srai_epi32(a32, 8), _mm_setzero_si128());
        __m128i b16 = _mm_packs_epi32(_mm_srai_epi32(b32, 8), _mm_setzero_si128());
        _mm_storeu_si128((__m128i*)(dst + *i), _mm_unpacklo_epi64(a16, b16));
    }
}

static inline void convert_24bit_to_16bit_simd(const unsigned char* src, short* dst,
                                               int sample_count) {
    CpuFeatures cpu = detect_cpu_features();
    int i = 0;

    if      (cpu.avx2) _cvt24to16_avx2(src, dst, &i, sample_count);
    else if (cpu.sse2) _cvt24to16_sse2(src, dst, &i, sample_count);

    _cvt24to16_scalar(src, dst, i, sample_count);
}


// ─────────────────────────────────────────────────────────────────────────────
// 32-bit signed → 16-bit signed  (right-shift 16)
//
// The AVX-512 path uses _mm512_cvtsepi32_epi16 — a single saturating
// truncation instruction — so it is worth keeping here, unlike the 8-bit case.
// ─────────────────────────────────────────────────────────────────────────────

static inline void _cvt32to16_scalar(const int* src, short* dst,
                                     int start, int end) {
    for (int i = start; i < end; i++)
        dst[i] = (short)(src[i] >> 16);
}

static inline void _cvt32to16_sse2(const int* src, short* dst,
                                   int* i, int n) {
    for (; *i <= n - 8; *i += 8) {
        __m128i a = _mm_srai_epi32(_mm_loadu_si128((const __m128i*)(src + *i)),     16);
        __m128i b = _mm_srai_epi32(_mm_loadu_si128((const __m128i*)(src + *i + 4)), 16);
        _mm_storeu_si128((__m128i*)(dst + *i), _mm_packs_epi32(a, b));
    }
}

static inline void _cvt32to16_avx2(const int* src, short* dst,
                                   int* i, int n) {
    for (; *i <= n - 16; *i += 16) {
        __m256i a      = _mm256_srai_epi32(
                             _mm256_loadu_si256((const __m256i*)(src + *i)),     16);
        __m256i b      = _mm256_srai_epi32(
                             _mm256_loadu_si256((const __m256i*)(src + *i + 8)), 16);
        __m256i packed = _mm256_packs_epi32(a, b);
        __m256i fixed  = _mm256_permute4x64_epi64(packed, 0xD8); // fix lane-crossing
        _mm256_storeu_si256((__m256i*)(dst + *i), fixed);
    }
}

#ifdef __AVX512F__
static inline void _cvt32to16_avx512(const int* src, short* dst,
                                     int* i, int n) {
    for (; *i <= n - 32; *i += 32) {
        __m512i a = _mm512_srai_epi32(
                        _mm512_loadu_si512((const __m512i*)(src + *i)),      16);
        __m512i b = _mm512_srai_epi32(
                        _mm512_loadu_si512((const __m512i*)(src + *i + 16)), 16);
        _mm256_storeu_si256((__m256i*)(dst + *i),      _mm512_cvtsepi32_epi16(a));
        _mm256_storeu_si256((__m256i*)(dst + *i + 16), _mm512_cvtsepi32_epi16(b));
    }
}
#endif

static inline void convert_32bit_to_16bit_simd(const int* src, short* dst,
                                               int sample_count) {
    CpuFeatures cpu = detect_cpu_features();
    int i = 0;

#ifdef __AVX512F__
    if (cpu.avx512f) _cvt32to16_avx512(src, dst, &i, sample_count);
#endif
    if      (cpu.avx2) _cvt32to16_avx2(src, dst, &i, sample_count);
    else if (cpu.sse2) _cvt32to16_sse2(src, dst, &i, sample_count);

    _cvt32to16_scalar(src, dst, i, sample_count);
}

// ─── WAV cache API ───────────────────────────────────────────────────────────

// Must be called with wavLock held.
static WAV* find_wav_data(const char* filename) {
    for (int i = 0; i < MAX_WAV_CACHE; i++) {
        if (g_audioSystem.wav_cache[i].loaded &&
            strcmp(g_audioSystem.wav_cache[i].filename, filename) == 0)
            return &g_audioSystem.wav_cache[i];
    }
    return NULL;
}

// All file I/O and sample conversion happen OUTSIDE the lock so loading a
// file never stalls the audio thread.
BOOL load_WAV(const char* filename) {
    if (!g_audioSystem.initialized) return false;

    // Quick check: already cached?
    EnterCriticalSection(&g_audioSystem.wavLock);
    if (find_wav_data(filename)) {
        LeaveCriticalSection(&g_audioSystem.wavLock);
        return true;
    }
    if (g_audioSystem.wav_cache_count >= MAX_WAV_CACHE) {
        LeaveCriticalSection(&g_audioSystem.wavLock);
        return false;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);

    // ── All I/O and conversion happen here, outside the lock ─────────────────
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "source/sound/%s", filename);

    FILE* file = fopen(full_path, "rb");
    if (!file) return false;

    RiffHeader riff;
    if (fread(&riff, sizeof(RiffHeader), 1, file) != 1 ||
        strncmp(riff.riff, "RIFF", 4) != 0 ||
        strncmp(riff.wave, "WAVE", 4) != 0) {
        fclose(file); return false;
    }

    FmtChunk       fmt      = {0};
    BOOL           got_fmt  = false;
    unsigned char* raw_data = NULL;
    UINT32         data_size = 0;

    ChunkHeader chunk;
    while (fread(&chunk, sizeof(ChunkHeader), 1, file) == 1) {
        if (strncmp(chunk.id, "fmt ", 4) == 0) {
            UINT32 read_size = chunk.size < sizeof(FmtChunk) ? chunk.size : sizeof(FmtChunk);
            if (fread(&fmt, read_size, 1, file) != 1) break;
            if (chunk.size > sizeof(FmtChunk))
                fseek(file, (long)(chunk.size - sizeof(FmtChunk) + (chunk.size & 1)), SEEK_CUR);
            got_fmt = true;
        } else if (strncmp(chunk.id, "data", 4) == 0) {
            data_size = chunk.size;
            raw_data  = (unsigned char*)malloc(data_size);
            if (!raw_data || fread(raw_data, data_size, 1, file) != 1) {
                free(raw_data); fclose(file); return false;
            }
            break;
        } else {
            // RIFF chunks are word-aligned: odd sizes carry a pad byte
            fseek(file, (long)(chunk.size + (chunk.size & 1)), SEEK_CUR);
        }
    }
    fclose(file);

    if (!got_fmt || !raw_data)                                          { free(raw_data); return false; }
    if (fmt.format != WAVE_FMT_PCM && fmt.format != WAVE_FMT_EXTENSIBLE){ free(raw_data); return false; }
    if (fmt.channels < 1 || fmt.channels > 2)                           { free(raw_data); return false; }
    if (fmt.bits_per_sample != 8  && fmt.bits_per_sample != 16 &&
        fmt.bits_per_sample != 24 && fmt.bits_per_sample != 32)         { free(raw_data); return false; }

    int bytes_per_sample = fmt.bits_per_sample / 8;
    int sample_count     = (int)(data_size / (fmt.channels * bytes_per_sample));
    int total_samples    = sample_count * fmt.channels;

    short* wav_data = (short*)malloc((size_t)total_samples * sizeof(short));
    if (!wav_data) { free(raw_data); return false; }

    // Conversion dispatches to the best available SIMD tier (AVX2 > SSE2 > scalar)
    if (fmt.bits_per_sample == 8) {
        convert_8bit_to_16bit_simd(raw_data, wav_data, total_samples);
    } else if (fmt.bits_per_sample == 16) {
        memcpy(wav_data, raw_data, (size_t)total_samples * sizeof(short));
    } else if (fmt.bits_per_sample == 24) {
        convert_24bit_to_16bit_simd(raw_data, wav_data, total_samples);
    } else {
        convert_32bit_to_16bit_simd((const int*)raw_data, wav_data, total_samples);
    }
    free(raw_data);

    // ── Commit to cache under lock ───────────────────────────────────────────
    EnterCriticalSection(&g_audioSystem.wavLock);

    // Re-check: another thread may have loaded the same file while we worked
    if (find_wav_data(filename)) {
        LeaveCriticalSection(&g_audioSystem.wavLock);
        free(wav_data);
        return true;
    }

    // Reuse the first free slot. Slots are never compacted, so WavSound
    // pointers into the cache stay valid for the lifetime of the load.
    WAV* wd = NULL;
    for (int i = 0; i < MAX_WAV_CACHE; i++) {
        if (!g_audioSystem.wav_cache[i].loaded) { wd = &g_audioSystem.wav_cache[i]; break; }
    }
    if (!wd) {
        LeaveCriticalSection(&g_audioSystem.wavLock);
        free(wav_data);
        return false;
    }

    wd->data         = wav_data;
    wd->sample_count = sample_count;
    wd->channels     = fmt.channels;
    wd->sample_rate  = (int)fmt.sample_rate;
    strncpy_s(wd->filename, sizeof(wd->filename), filename, _TRUNCATE);
    wd->loaded = true;
    g_audioSystem.wav_cache_count++;

    LeaveCriticalSection(&g_audioSystem.wavLock);
    return true;
}

void unload_WAV(const char* filename) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.wavLock);

    WAV* target = find_wav_data(filename);
    if (target) {
        // Nullify any live WavSound slots that point at this data so the
        // mixer never dereferences a freed buffer.
        for (int j = 0; j < MAX_WAV_SOUNDS; j++) {
            if (g_audioSystem.wav[j].wav_data == target) {
                g_audioSystem.wav[j].active   = false;
                g_audioSystem.wav[j].wav_data = NULL;
            }
        }

        free(target->data);
        target->data        = NULL;
        target->loaded      = false;
        target->filename[0] = '\0';
        g_audioSystem.wav_cache_count--;

        printf("Unloaded WAV file: %s\n", filename);
    }

    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void unload_all_WAVs(void) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.wavLock);

    for (int j = 0; j < MAX_WAV_SOUNDS; j++) {
        g_audioSystem.wav[j].active   = false;
        g_audioSystem.wav[j].wav_data = NULL;
    }

    for (int i = 0; i < MAX_WAV_CACHE; i++) {
        if (g_audioSystem.wav_cache[i].loaded && g_audioSystem.wav_cache[i].data)
            free(g_audioSystem.wav_cache[i].data);
        g_audioSystem.wav_cache[i].data        = NULL;
        g_audioSystem.wav_cache[i].loaded      = false;
        g_audioSystem.wav_cache[i].filename[0] = '\0';
    }
    g_audioSystem.wav_cache_count = 0;

    printf("Unloaded all WAV files from cache\n");
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

// ─── WAV sound API ───────────────────────────────────────────────────────────

// Normalize an external WAV id (100..199 or bare 0..99) to a slot index,
// or -1 if out of range.
static inline int wav_slot_from_id(int id) {
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) id -= 100;
    if (id < 0 || id >= MAX_WAV_SOUNDS) return -1;
    return id;
}

// Must be called with wavLock held. Explicit id claims that slot (restarting
// whatever occupied it); any other id auto-picks the first free slot.
static int get_or_create_wav_sound(int id, WAV* wav_data, float amplitude) {
    int slot = wav_slot_from_id(id);
    if (slot < 0) {
        for (int i = 0; i < MAX_WAV_SOUNDS; i++)
            if (!g_audioSystem.wav[i].active) { slot = i; break; }
        if (slot < 0) return -1;
    }
    init_wav_sound_common(&g_audioSystem.wav[slot], slot, wav_data, amplitude);
    return slot;
}

int play_WAV_by_duration(int id, const char* filename, float amplitude, double duration_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int slot = get_or_create_wav_sound(id, wd, amplitude);
    if (slot >= 0) {
        g_audioSystem.wav[slot].fade_state    = FADE_TIMED;
        g_audioSystem.wav[slot].timer_samples = (int)(duration_seconds * SAMPLE_RATE);
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return slot >= 0 ? slot + 100 : -1;
}

int play_repeating_WAV(int id, const char* filename, float amplitude) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int slot = get_or_create_wav_sound(id, wd, amplitude);
    if (slot >= 0) {
        g_audioSystem.wav[slot].fade_state = FADE_IN;
        g_audioSystem.wav[slot].repeat     = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return slot >= 0 ? slot + 100 : -1;
}

int play_delayed_WAV_by_duration(int id, const char* filename, float amplitude,
                                 double duration_seconds, double start_delay_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int slot = get_or_create_wav_sound(id, wd, amplitude);
    if (slot >= 0) {
        g_audioSystem.wav[slot].fade_state               = FADE_DELAY;
        g_audioSystem.wav[slot].delay_samples            = (int)(start_delay_seconds * SAMPLE_RATE);
        g_audioSystem.wav[slot].is_timed_after_delay     = true;
        g_audioSystem.wav[slot].delayed_duration_seconds = duration_seconds;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return slot >= 0 ? slot + 100 : -1;
}

int play_delayed_repeating_WAV(int id, const char* filename, float amplitude,
                               double start_delay_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int slot = get_or_create_wav_sound(id, wd, amplitude);
    if (slot >= 0) {
        g_audioSystem.wav[slot].fade_state           = FADE_DELAY;
        g_audioSystem.wav[slot].delay_samples        = (int)(start_delay_seconds * SAMPLE_RATE);
        g_audioSystem.wav[slot].is_timed_after_delay = false;
        g_audioSystem.wav[slot].repeat               = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return slot >= 0 ? slot + 100 : -1;
}

// Delay, then retrigger the WAV from the start every play_duration_seconds
// until stopped (original behavior, kept intentionally).
int play_repeating_delayed_WAV_by_duration(int id, const char* filename, float amplitude,
                                           double play_duration_seconds, double start_delay_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int slot = get_or_create_wav_sound(id, wd, amplitude);
    if (slot >= 0) {
        g_audioSystem.wav[slot].fade_state               = FADE_DELAY;
        g_audioSystem.wav[slot].delay_samples            = (int)(start_delay_seconds * SAMPLE_RATE);
        g_audioSystem.wav[slot].is_timed_after_delay     = true;
        g_audioSystem.wav[slot].delayed_duration_seconds = play_duration_seconds;
        g_audioSystem.wav[slot].repeat                   = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return slot >= 0 ? slot + 100 : -1;
}

int play_specific_part_WAV(int id, const char* filename, float amplitude, int start_sample, int end_sample) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    if (start_sample < 0 || end_sample <= start_sample || end_sample > wd->sample_count) {
        LeaveCriticalSection(&g_audioSystem.wavLock);
        return -1;
    }

    int slot = get_or_create_wav_sound(id, wd, amplitude);
    if (slot >= 0) {
        g_audioSystem.wav[slot].current_position = start_sample;
        g_audioSystem.wav[slot].loop_end_sample  = end_sample;
        g_audioSystem.wav[slot].fade_state       = FADE_SUSTAIN;  // instant attack, fades at segment end
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return slot >= 0 ? slot + 100 : -1;
}

int play_repeating_specific_part_WAV(int id, const char* filename, float amplitude, int start_sample, int end_sample) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    if (start_sample < 0 || end_sample <= start_sample || end_sample > wd->sample_count) {
        LeaveCriticalSection(&g_audioSystem.wavLock);
        return -1;
    }

    int slot = get_or_create_wav_sound(id, wd, amplitude);
    if (slot >= 0) {
        g_audioSystem.wav[slot].current_position  = start_sample;
        g_audioSystem.wav[slot].loop_start_sample = start_sample;
        g_audioSystem.wav[slot].loop_end_sample   = end_sample;
        g_audioSystem.wav[slot].fade_state        = FADE_SUSTAIN;
        g_audioSystem.wav[slot].repeat            = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return slot >= 0 ? slot + 100 : -1;
}

void stop_WAV(int id) {
    if (!g_audioSystem.initialized) return;
    int slot = wav_slot_from_id(id);
    if (slot < 0) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WavSound* sound = &g_audioSystem.wav[slot];
    if (sound->active) {
        if (sound->fade_state == FADE_DELAY || sound->paused) {
            sound->active = false;      // silent right now — no fade needed
            sound->paused = false;
        } else if (sound->fade_state != FADE_OUT) {
            sound->fade_state   = FADE_OUT;
            sound->fade_counter = 0;
        }
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void stop_all_WAVs(void) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    for (int i = 0; i < MAX_WAV_SOUNDS; i++) {
        WavSound* sound = &g_audioSystem.wav[i];
        if (!sound->active) continue;
        if (sound->fade_state == FADE_DELAY || sound->paused) {
            sound->active = false;
            sound->paused = false;
        } else if (sound->fade_state != FADE_OUT) {
            sound->fade_state   = FADE_OUT;
            sound->fade_counter = 0;
        }
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void set_amp_WAV(int id, float amplitude) {
    if (!g_audioSystem.initialized) return;
    int slot = wav_slot_from_id(id);
    if (slot < 0) return;

    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;

    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[slot].active)
        g_audioSystem.wav[slot].amplitude = amplitude;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void pause_WAV(int id) {
    if (!g_audioSystem.initialized) return;
    int slot = wav_slot_from_id(id);
    if (slot < 0) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[slot].active)
        g_audioSystem.wav[slot].paused = true;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void resume_WAV(int id) {
    if (!g_audioSystem.initialized) return;
    int slot = wav_slot_from_id(id);
    if (slot < 0) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[slot].active)
        g_audioSystem.wav[slot].paused = false;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void set_pitch_WAV(int id, float pitch) {
    if (!g_audioSystem.initialized) return;
    int slot = wav_slot_from_id(id);
    if (slot < 0) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[slot].active)
        g_audioSystem.wav[slot].pitch = (pitch > 0.01f) ? pitch : 0.01f;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void set_fade_duration_WAV(int id, int samples) {
    if (!g_audioSystem.initialized) return;
    int slot = wav_slot_from_id(id);
    if (slot < 0) return;
    if (samples < 1) samples = 1;   // 0 would divide by zero in the envelope

    EnterCriticalSection(&g_audioSystem.wavLock);
    g_audioSystem.wav[slot].fade_duration = samples;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

BOOL check_active_WAV(int id) {
    if (!g_audioSystem.initialized) return false;
    int slot = wav_slot_from_id(id);
    if (slot < 0) return false;

    EnterCriticalSection(&g_audioSystem.wavLock);
    BOOL playing = g_audioSystem.wav[slot].active;
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return playing;
}

WAVInfo get_WAV_info(const char* filename) {
    WAVInfo info = {0, 0};

    if (!g_audioSystem.initialized) return info;
    if (!load_WAV(filename)) return info;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (wd) {
        info.sample_rate   = wd->sample_rate;
        info.total_samples = wd->sample_count;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);

    return info;
}
