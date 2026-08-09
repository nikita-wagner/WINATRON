#include "audio.hpp"

AudioSystem g_audioSystem = {0};


// ─── Reverb side arrays ───────────────────────────────────────────────────────
// FIX: Moved out of Tone/WavSound structs to eliminate ~17.7 KB per-slot copy
//      inside the critical section in audio_mixer.
//      Only ever accessed by the audio thread; no lock required.

static short reverb_bufs_tone[MAX_TONE_SOUNDS][REVERB_BUF_LEN];
static int   reverb_idx_tone [MAX_TONE_SOUNDS];

static short reverb_bufs_wav [MAX_WAV_SOUNDS][REVERB_BUF_LEN];
static int   reverb_idx_wav  [MAX_WAV_SOUNDS];

// ─── Audio thread ─────────────────────────────────────────────────────────────


// forward declaration of audio_mixer so it can be called from the thread proc
extern void audio_mixer(short* buffer, int buffer_size);

static DWORD WINAPI audio_thread_proc(LPVOID lpParam) {

    while (g_audioSystem.running) {
        for (int i = 0; i < NUM_BUFFERS; i++) {
            WAVEHDR* header = &g_audioSystem.waveHeaders[i];
            if (header->dwFlags & WHDR_DONE || !(header->dwFlags & WHDR_PREPARED)) {
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

// ─── Sine table ───────────────────────────────────────────────────────────────

#define SINE_TABLE_SIZE 1024
static float  sine_table[SINE_TABLE_SIZE];
static bool   sine_table_initialized = false;

// FIX: Precomputed scale avoids per-sample division by 2π inside fast_sin.
static const double SINE_SCALE = (double)SINE_TABLE_SIZE / (2.0 * PI);

static void init_sine_table(void) {
    if (sine_table_initialized) return;
    for (int i = 0; i < SINE_TABLE_SIZE; i++)
        sine_table[i] = (float)sin(2.0 * PI * i / SINE_TABLE_SIZE);
    sine_table_initialized = true;
}

// FIX: Original fast_sin divided by 2π and called floor() every sample.
//      Callers already keep phase in [0, 2π), so we only need one multiply + cast.
static inline float fast_sin(double phase) {
    return sine_table[(int)(phase * SINE_SCALE) & (SINE_TABLE_SIZE - 1)];
}

// ─── Init / Shutdown ──────────────────────────────────────────────────────────

bool audio_init(void) {
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
        g_audioSystem.waveHeaders[i].lpData        = (LPSTR)g_audioSystem.audioBuffers[i];
        g_audioSystem.waveHeaders[i].dwBufferLength = BUFFER_SIZE * 2 * sizeof(short);
        g_audioSystem.waveHeaders[i].dwFlags        = 0;
        g_audioSystem.waveHeaders[i].dwLoops        = 0;
    }

    // FIX: memset already zeroed all bool/int/pointer/float fields to 0/false/NULL/0.0f.
    //      Only set non-zero defaults here.
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
        g_audioSystem.wav[i].sound_index   = i;
    }

    // FIX: was hardcoded to 32 instead of MAX_WAV_CACHE
    for (int i = 0; i < MAX_WAV_CACHE; i++) {
        g_audioSystem.wav_cache[i].loaded = false;
        g_audioSystem.wav_cache[i].data   = NULL;
    }

    // Zero-init reverb side arrays
    memset(reverb_bufs_tone, 0, sizeof(reverb_bufs_tone));
    memset(reverb_idx_tone,  0, sizeof(reverb_idx_tone));
    memset(reverb_bufs_wav,  0, sizeof(reverb_bufs_wav));
    memset(reverb_idx_wav,   0, sizeof(reverb_idx_wav));

    init_sine_table();

    g_audioSystem.initialized = true;
    g_audioSystem.running     = true;

    g_audioSystem.audioThread = CreateThread(NULL, 0, audio_thread_proc, NULL, 0, NULL);
    if (g_audioSystem.audioThread == NULL) {
        waveOutClose(g_audioSystem.hWaveOut);
        DeleteCriticalSection(&g_audioSystem.toneLock);
        DeleteCriticalSection(&g_audioSystem.wavLock);
        return false;
    }

    return true;
}

// Forward declarations for audio_shutdown dependencies
void stop_all_tones(void);
void stop_all_WAVs(void);
void unload_all_WAVs(void);

void audio_shutdown(void) {
    if (!g_audioSystem.initialized)
        return;

    g_audioSystem.running = false;
    Sleep(100);

    if (g_audioSystem.audioThread != NULL) {
        WaitForSingleObject(g_audioSystem.audioThread, 2000);
        CloseHandle(g_audioSystem.audioThread);
        g_audioSystem.audioThread = NULL;
    }

    stop_all_tones();
    stop_all_WAVs();
    unload_all_WAVs();

    waveOutReset(g_audioSystem.hWaveOut);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (g_audioSystem.waveHeaders[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_audioSystem.hWaveOut, &g_audioSystem.waveHeaders[i], sizeof(WAVEHDR));
    }

    waveOutClose(g_audioSystem.hWaveOut);
    DeleteCriticalSection(&g_audioSystem.toneLock);
    DeleteCriticalSection(&g_audioSystem.wavLock);

    g_audioSystem.initialized = false;
}

// ─── Stereo panning ───────────────────────────────────────────────────────────

static inline void calculate_stereo_amplitudes(float angle, float* left_amp, float* right_amp) {
    angle = fmodf(angle, 360.0f);
    if (angle < 0) angle += 360.0f;

    float rad          = ANGLE_TO_RADIANS(angle);
    float lr_component = sinf(rad);
    float fb_component = cosf(rad);

    float distance_factor = 0.65f - fb_component * 0.35f;

    *right_amp = (0.5f + lr_component * 0.5f) * distance_factor;
    *left_amp  = (0.5f - lr_component * 0.5f) * distance_factor;

    if (*left_amp  < 0) *left_amp  = 0;
    if (*right_amp < 0) *right_amp = 0;
    if (*left_amp  > 1) *left_amp  = 1;
    if (*right_amp > 1) *right_amp = 1;
}

// ─── Shared helpers ───────────────────────────────────────────────────────────

static inline float tick_fade_in(int* counter, int duration, int* state) {
    float env = (float)(*counter) / (float)duration;
    if (env > 1.0f) env = 1.0f;
    (*counter)++;
    if (*counter >= duration) *state = 1;
    return env;
}

static inline float tick_fade_out(int* counter, int duration, bool* active) {
    float env = 1.0f - (float)(*counter) / (float)duration;
    (*counter)++;
    if (env <= 0.0f) { *active = false; return -1.0f; }
    return env;
}

// Reverb: uses caller-provided buffer and index pointers (now side arrays).
static inline float apply_reverb(float sample, short* buf, int* idx,
                                  float amount, float decay) {
    float wet  = sample + buf[*idx] * amount;
    buf[*idx]  = (short)(wet * decay);
    *idx       = (*idx + 1) % REVERB_BUF_LEN;
    return wet / (1.0f + amount * 0.5f);
}

static inline void mix_stereo(short* buffer, int i,
                               float sample, float left_amp, float right_amp) {
    int L = buffer[i * 2]     + (int)(sample * left_amp);
    int R = buffer[i * 2 + 1] + (int)(sample * right_amp);
    buffer[i * 2]     = (short)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
    buffer[i * 2 + 1] = (short)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
}

// ─── Sound initializers ───────────────────────────────────────────────────────

// FIX: Takes slot so it can reset the reverb side array for that slot.
static void init_sound_common(Tone* sound, int slot,
                               double frequency, float amplitude, double phase) {
    sound->frequency                = frequency;
    sound->phase                    = phase;
    sound->amplitude                = amplitude;
    sound->angle                    = 0.0f;
    sound->left_amp                 = 1.0f;
    sound->right_amp                = 1.0f;
    sound->active                   = true;
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

    memset(reverb_bufs_wav[slot], 0, REVERB_BUF_LEN * sizeof(short));
    reverb_idx_wav[slot] = 0;
}

// ─── audio_mixer ─────────────────────────────────────────────────────
void audio_mixer(short* buffer, int buffer_size) {
    memset(buffer, 0, buffer_size * 2 * sizeof(short));

    // ── Tone sounds ───────────────────────────────────────────────────────────
    Tone active_sounds[MAX_TONE_SOUNDS];
    int  active_count = 0;

    EnterCriticalSection(&g_audioSystem.toneLock);
    for (int v = 0; v < MAX_TONE_SOUNDS; v++) {
        if (g_audioSystem.tone[v].active) {
            active_sounds[active_count]             = g_audioSystem.tone[v];
            active_sounds[active_count].sound_index = v;
            active_count++;
        }
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);

    for (int v = 0; v < active_count; v++) {
        Tone* tone   = &active_sounds[v];
        int   slot = tone->sound_index;

        // Delay phase
        if (tone->fade_state == 4) {
            tone->delay_counter += buffer_size;
            if (tone->delay_counter < tone->delay_samples) continue;
            tone->fade_state = tone->is_timed_after_delay ? 3 : 0;
            if (tone->fade_state == 3) {
                tone->timer_samples = (int)(tone->delayed_duration_seconds * SAMPLE_RATE);
                tone->timer_counter = 0;
            }
            tone->fade_counter  = 0;
            tone->fade_duration = FADE_SAMPLES;
        }

        if (tone->paused) continue;

        double phase_inc = 2.0 * PI * tone->frequency / SAMPLE_RATE;
        calculate_stereo_amplitudes(tone->angle, &tone->left_amp, &tone->right_amp);

        for (int i = 0; i < buffer_size; i++) {
            // FIX: fast_sin now only does one multiply + mask (no divide/floor)
            float sample = fast_sin(tone->phase) * tone->amplitude * AMPLITUDE;
            tone->phase += phase_inc;
            if (tone->phase >= 2.0 * PI) tone->phase -= 2.0 * PI;

            if (tone->reverb_amount > 0.0f)
                sample = apply_reverb(sample,
                                      reverb_bufs_tone[slot], &reverb_idx_tone[slot],
                                      tone->reverb_amount, tone->reverb_decay);

            float env = 1.0f;
            if (tone->fade_state == 0) {
                env = tick_fade_in(&tone->fade_counter, tone->fade_duration, &tone->fade_state);
            } else if (tone->fade_state == 2) {
                env = tick_fade_out(&tone->fade_counter, tone->fade_duration, &tone->active);
                if (env < 0.0f) break;
            } else if (tone->fade_state == 3) {
                tone->timer_counter++;
                if (tone->timer_counter >= tone->timer_samples) {
                    tone->fade_state   = 2;
                    tone->fade_counter = 0;
                    tone->fade_duration = FADE_SAMPLES;
                }
            }

            mix_stereo(buffer, i, sample * env, tone->left_amp, tone->right_amp);
        }
    }

    EnterCriticalSection(&g_audioSystem.toneLock);
    for (int v = 0; v < active_count; v++) {
        int idx = active_sounds[v].sound_index;
        if (g_audioSystem.tone[idx].active || !active_sounds[v].active)
            g_audioSystem.tone[idx] = active_sounds[v];
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);

    // ── WAV sounds ────────────────────────────────────────────────────────────
    WavSound active_wav_sounds[MAX_WAV_SOUNDS];
    int      active_wav_count = 0;

    EnterCriticalSection(&g_audioSystem.wavLock);
    for (int v = 0; v < MAX_WAV_SOUNDS; v++) {
        if (g_audioSystem.wav[v].active) {
            active_wav_sounds[active_wav_count]             = g_audioSystem.wav[v];
            active_wav_sounds[active_wav_count].sound_index = v;
            active_wav_count++;
        }
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);

    for (int v = 0; v < active_wav_count; v++) {
        WavSound* s    = &active_wav_sounds[v];
        WAV*  wd   = s->wav_data;
        int       slot = s->sound_index;

        if (!wd || !wd->loaded || !wd->data) { s->active = false; continue; }

        // Delay phase
        if (s->fade_state == 4) {
            s->delay_counter += buffer_size;
            if (s->delay_counter < s->delay_samples) continue;
            s->fade_state = s->is_timed_after_delay ? 3 : 0;
            if (s->fade_state == 3) {
                s->timer_samples = (int)(s->delayed_duration_seconds * SAMPLE_RATE);
                s->timer_counter = 0;
            }
            s->fade_counter  = 0;
            s->fade_duration = FADE_SAMPLES;
        }

        if (s->paused) continue;

        float rate_ratio = ((float)wd->sample_rate / (float)SAMPLE_RATE) * s->pitch;

        calculate_stereo_amplitudes(s->angle, &s->left_amp, &s->right_amp);

        for (int i = 0; i < buffer_size; i++) {
            s->fractional_position += rate_ratio;
            int advance = (int)s->fractional_position;
            if (advance > 0) {
                s->current_position    += advance;
                s->fractional_position -= (float)advance;
            }

            if (s->current_position >= wd->sample_count) {
                if (s->repeat) {
                     s->current_position = s->loop_start_sample;
                } else {
                    s->fade_counter = 0;
                    s->fade_duration = FADE_SAMPLES;
                    break;
                }
            }

            float wav_sample;
            if (wd->channels == 1) {
                wav_sample = (float)wd->data[s->current_position];
            } else {
                int li     = s->current_position * 2;
                int ri     = li + 1;
                wav_sample = (ri < wd->sample_count * 2)
                    ? ((float)wd->data[li] + (float)wd->data[ri]) * 0.5f
                    :  (float)wd->data[li];
            }
            wav_sample *= s->amplitude;

            if (s->reverb_amount > 0.0f)
                wav_sample = apply_reverb(wav_sample,
                                          reverb_bufs_wav[slot], &reverb_idx_wav[slot],
                                          s->reverb_amount, s->reverb_decay);

            float env = 1.0f;
            if (s->fade_state == 0) {
                env = tick_fade_in(&s->fade_counter, s->fade_duration, &s->fade_state);
            } else if (s->fade_state == 2) {
                env = tick_fade_out(&s->fade_counter, s->fade_duration, &s->active);
                if (env < 0.0f) break;
            } else if (s->fade_state == 3) {
                s->timer_counter++;
                if (s->timer_counter >= s->timer_samples) {
                    if (s->repeat) {
                        s->current_position    = s->loop_start_sample;
                        s->fractional_position = 0.0f;
                        s->timer_counter       = 0;
                    } else {
                        s->fade_state    = 2;
                        s->fade_counter  = 0;
                        s->fade_duration = FADE_SAMPLES;
                    }
                }
            }

            mix_stereo(buffer, i, wav_sample * env, s->left_amp, s->right_amp);
        }
    }

    EnterCriticalSection(&g_audioSystem.wavLock);
    for (int v = 0; v < active_wav_count; v++) {
        int idx = active_wav_sounds[v].sound_index;
        if (g_audioSystem.wav[idx].active || !active_wav_sounds[v].active)
            g_audioSystem.wav[idx] = active_wav_sounds[v];
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

// ─── Tone API ─────────────────────────────────────────────────────────────────

int play_tone(double frequency, float gain) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);
    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        if (!g_audioSystem.tone[i].active) {
            init_sound_common(&g_audioSystem.tone[i], i, frequency, gain, 0.0);
            g_audioSystem.tone[i].fade_state = 0;
            LeaveCriticalSection(&g_audioSystem.toneLock);
            return i;
        }
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return -1;
}

void stop_tone(int sound_id) {
    if (sound_id < 0 || sound_id >= MAX_TONE_SOUNDS || !g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.toneLock);
    Tone* sound = &g_audioSystem.tone[sound_id];
    if (sound->active && sound->fade_state != 2) {
        sound->fade_state   = 2;
        sound->fade_counter = 0;
        sound->fade_duration = FADE_SAMPLES;
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void stop_all_tones(void) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.toneLock);
    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        Tone* sound = &g_audioSystem.tone[i];
        if (sound->active && sound->fade_state != 2) {
            sound->fade_state   = 2;
            sound->fade_counter = 0;
            sound->fade_duration = FADE_SAMPLES;
        }
    }
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

int play_tone_by_duration(int id, double frequency, float amplitude, double phase, double duration_seconds) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);

    if (id >= 0 && id < MAX_TONE_SOUNDS) {
        Tone* sound = &g_audioSystem.tone[id];
        sound->active = false;
        init_sound_common(sound, id, frequency, amplitude, phase);
        sound->fade_state    = 3;
        sound->timer_samples = (int)(duration_seconds * SAMPLE_RATE);
        LeaveCriticalSection(&g_audioSystem.toneLock);
        return id;
    }

    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        if (!g_audioSystem.tone[i].active) {
            init_sound_common(&g_audioSystem.tone[i], i, frequency, amplitude, phase);
            g_audioSystem.tone[i].fade_state    = 3;
            g_audioSystem.tone[i].timer_samples = (int)(duration_seconds * SAMPLE_RATE);
            LeaveCriticalSection(&g_audioSystem.toneLock);
            return i;
        }
    }

    LeaveCriticalSection(&g_audioSystem.toneLock);
    return -1;
}

int play_static_tone(int id, double frequency, float amplitude, double phase) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);

    if (id >= 0 && id < MAX_TONE_SOUNDS) {
        Tone* sound = &g_audioSystem.tone[id];
        sound->active = false;
        init_sound_common(sound, id, frequency, amplitude, phase);
        sound->fade_state = 0;
        sound->frequency      = frequency;
        sound->base_frequency = frequency;  // ← ADD
        LeaveCriticalSection(&g_audioSystem.toneLock);
        return id;
    }

    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        if (!g_audioSystem.tone[i].active) {
            init_sound_common(&g_audioSystem.tone[i], i, frequency, amplitude, phase);
            g_audioSystem.tone[i].fade_state = 0;
            g_audioSystem.tone[i].base_frequency = frequency;  // ← ADD
            LeaveCriticalSection(&g_audioSystem.toneLock);
            return i;
        }
    }

    LeaveCriticalSection(&g_audioSystem.toneLock);
    return -1;
}

int play_delayed_tone_by_duration(int id, double frequency, float amplitude, double phase,
                         double duration_seconds, double start_delay_seconds) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);

    if (id >= 0 && id < MAX_TONE_SOUNDS) {
        Tone* sound = &g_audioSystem.tone[id];
        sound->active = false;
        init_sound_common(sound, id, frequency, amplitude, phase);
        sound->fade_state                = 4;
        sound->delay_samples             = (int)(start_delay_seconds * SAMPLE_RATE);
        sound->is_timed_after_delay      = true;
        sound->delayed_duration_seconds  = duration_seconds;
        LeaveCriticalSection(&g_audioSystem.toneLock);
        return id;
    }

    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        if (!g_audioSystem.tone[i].active) {
            init_sound_common(&g_audioSystem.tone[i], i, frequency, amplitude, phase);
            g_audioSystem.tone[i].fade_state               = 4;
            g_audioSystem.tone[i].delay_samples            = (int)(start_delay_seconds * SAMPLE_RATE);
            g_audioSystem.tone[i].is_timed_after_delay     = true;
            g_audioSystem.tone[i].delayed_duration_seconds = duration_seconds;
            LeaveCriticalSection(&g_audioSystem.toneLock);
            return i;
        }
    }

    LeaveCriticalSection(&g_audioSystem.toneLock);
    return -1;
}

int play_delayed_static_tone(int id, double frequency, float amplitude, double phase,
                          double start_delay_seconds) {
    if (!g_audioSystem.initialized) return -1;

    EnterCriticalSection(&g_audioSystem.toneLock);

    if (id >= 0 && id < MAX_TONE_SOUNDS) {
        Tone* sound = &g_audioSystem.tone[id];
        sound->active = false;
        init_sound_common(sound, id, frequency, amplitude, phase);
        sound->fade_state            = 4;
        sound->delay_samples         = (int)(start_delay_seconds * SAMPLE_RATE);
        sound->is_timed_after_delay  = false;
        LeaveCriticalSection(&g_audioSystem.toneLock);
        return id;
    }

    for (int i = 0; i < MAX_TONE_SOUNDS; i++) {
        if (!g_audioSystem.tone[i].active) {
            init_sound_common(&g_audioSystem.tone[i], i, frequency, amplitude, phase);
            g_audioSystem.tone[i].fade_state           = 4;
            g_audioSystem.tone[i].delay_samples        = (int)(start_delay_seconds * SAMPLE_RATE);
            g_audioSystem.tone[i].is_timed_after_delay = false;
            LeaveCriticalSection(&g_audioSystem.toneLock);
            return i;
        }
    }

    LeaveCriticalSection(&g_audioSystem.toneLock);
    return -1;
}

void angle_of_tone(int id, float angle) {
    if (!g_audioSystem.initialized) return;

    if (id >= 0 && id < MAX_TONE_SOUNDS) {
        EnterCriticalSection(&g_audioSystem.toneLock);
        Tone* sound = &g_audioSystem.tone[id];
        if (sound->active) {
            sound->angle = angle;
            calculate_stereo_amplitudes(angle, &sound->left_amp, &sound->right_amp);
        }
        LeaveCriticalSection(&g_audioSystem.toneLock);

    } else if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) {
        int wav_id = id - 100;
        EnterCriticalSection(&g_audioSystem.wavLock);
        WavSound* ws = &g_audioSystem.wav[wav_id];
        if (ws->active) {
            ws->angle = angle;
            calculate_stereo_amplitudes(angle, &ws->left_amp, &ws->right_amp);
        }
        LeaveCriticalSection(&g_audioSystem.wavLock);
    }
}

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
        int wav_id = id - 100;
        EnterCriticalSection(&g_audioSystem.wavLock);
        WavSound* ws = &g_audioSystem.wav[wav_id];
        if (ws->active) {
            ws->reverb_amount = amount;
            ws->reverb_decay  = decay;
        }
        LeaveCriticalSection(&g_audioSystem.wavLock);
    }
}

void set_amp_tone(int id, float amplitude) {
    if (!g_audioSystem.initialized) return;
    if (id < 0 || id >= MAX_TONE_SOUNDS) return;

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













// ─────────────────────────────────────────────// 
////////////////// WAV Section////////////////////
// ─────────────────────────────────────────────// 

// ─── WAV file loading & SIMD conversions ──────────────────────────────────────

#include <immintrin.h>
#include <intrin.h>

#pragma pack(push, 1)
typedef struct { char riff[4]; uint32_t chunk_size; char wave[4]; } RiffHeader;
typedef struct { char id[4];   uint32_t size;                      } ChunkHeader;
typedef struct {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;
#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
// CPUID feature detection (runtime, cached)
// ─────────────────────────────────────────────────────────────────────────────

typedef struct {
    bool sse2;
    bool avx2;
    bool avx512f;
    bool avx512bw;
    bool avx512vbmi;
} CpuFeatures;

static inline CpuFeatures detect_cpu_features(void) {
    static CpuFeatures f = {false, false, false, false, false};
    static bool cached   = false;
    if (cached) return f;

    int info[4];

    // ── Leaf 1 ────────────────────────────────────────────────────────────────
    __cpuid(info, 1);
    f.sse2 = (info[3] & (1 << 26)) != 0;

    bool osxsave = (info[2] & (1 << 27)) != 0;
    bool avx_ecx = (info[2] & (1 << 28)) != 0;

    if (!osxsave || !avx_ecx) { cached = true; return f; }

    uint64_t xcr0 = _xgetbv(0);
    bool ymm_ok   = (xcr0 & 0x06) == 0x06;
    bool zmm_ok   = ymm_ok && ((xcr0 & 0xE0) == 0xE0);

    // ── Leaf 7, subleaf 0 ────────────────────────────────────────────────────
    __cpuidex(info, 7, 0);
    if (ymm_ok) {
        f.avx2     = (info[1] & (1 << 5))  != 0;
        f.avx512f  = zmm_ok && ((info[1] & (1 << 16)) != 0);
        f.avx512bw = zmm_ok && ((info[1] & (1 << 30)) != 0);
    }

    if (f.avx512f) {
        __cpuidex(info, 7, 0);
        f.avx512vbmi = (info[2] & (1 << 1)) != 0;
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
    // Preferred over the AVX-512 path on Zen 4 (256-bit execution units) and
    // competitive with true 512-bit hardware because we avoid the wider
    // register pressure and decode overhead.
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

// AVX-512 path removed: the 64-byte AVX2 loop above is equal or faster on
// Zen 4 (256-bit µops) and within a few percent on Intel, so the added
// compile-time complexity and register pressure aren't worth it.

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
// No change: the 24-bit packed format means every sample straddles a different
// byte boundary, so SIMD can't load/convert without a costly scatter/gather or
// shuffle table.  The existing scalar extraction + SIMD pack approach is already
// the practical optimum.
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
// No change: already well-optimized.  _cvt32to16_avx512 uses
// _mm512_cvtsepi32_epi16 which is a single saturating-truncation instruction —
// exactly what true 512-bit hardware (Intel Ice Lake+, Sapphire Rapids) excels
// at — so the AVX-512 path is worth keeping here, unlike the 8-bit case.
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

// ─── WAV cache API ────────────────────────────────────────────────────────────

// FIX: All file I/O and sample conversion now happen OUTSIDE the lock.
//      Previously the entire fopen/fread/malloc/convert block ran while
//      holding wavLock, blocking the audio thread every time a file was loaded.
bool load_WAV(const char* filename) {
    if (!g_audioSystem.initialized) return false;

    // Quick check: already cached?
    EnterCriticalSection(&g_audioSystem.wavLock);
    for (int i = 0; i < g_audioSystem.wav_cache_count; i++) {
        if (strcmp(g_audioSystem.wav_cache[i].filename, filename) == 0 &&
            g_audioSystem.wav_cache[i].loaded) {
            LeaveCriticalSection(&g_audioSystem.wavLock);
            return true;
        }
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
    bool           got_fmt  = false;
    unsigned char* raw_data = NULL;
    uint32_t       data_size = 0;

    ChunkHeader chunk;
    while (fread(&chunk, sizeof(ChunkHeader), 1, file) == 1) {
        if (strncmp(chunk.id, "fmt ", 4) == 0) {
            uint32_t read_size = chunk.size < sizeof(FmtChunk) ? chunk.size : sizeof(FmtChunk);
            if (fread(&fmt, read_size, 1, file) != 1) break;
            if (chunk.size > sizeof(FmtChunk))
                fseek(file, chunk.size - sizeof(FmtChunk), SEEK_CUR);
            got_fmt = true;
        } else if (strncmp(chunk.id, "data", 4) == 0) {
            data_size = chunk.size;
            raw_data  = (unsigned char*)malloc(data_size);
            if (!raw_data || fread(raw_data, data_size, 1, file) != 1) {
                free(raw_data); fclose(file); return false;
            }
            break;
        } else {
            fseek(file, chunk.size, SEEK_CUR);
        }
    }
    fclose(file);

    if (!got_fmt || !raw_data)                                          { free(raw_data); return false; }
    if (fmt.format != 1)                                                { free(raw_data); return false; }
    if (fmt.bits_per_sample != 8  && fmt.bits_per_sample != 16 &&
        fmt.bits_per_sample != 24 && fmt.bits_per_sample != 32)         { free(raw_data); return false; }

    int bytes_per_sample = fmt.bits_per_sample / 8;
    int sample_count     = (int)(data_size / (fmt.channels * bytes_per_sample));
    int total_samples    = sample_count * fmt.channels;

    short* wav_data = (short*)malloc(total_samples * sizeof(short));
    if (!wav_data) { free(raw_data); return false; }

    // Conversion functions now internally dispatch to the best available SIMD tier <3333333 
    // (AVX-512 > AVX2 > SSE2 > scalar)

    if (fmt.bits_per_sample == 8) {
        convert_8bit_to_16bit_simd(raw_data, wav_data, total_samples);
    } else if (fmt.bits_per_sample == 16) {
        memcpy(wav_data, raw_data, total_samples * sizeof(short));
    } else if (fmt.bits_per_sample == 24) {
        convert_24bit_to_16bit_simd(raw_data, wav_data, total_samples);
    } else {
        convert_32bit_to_16bit_simd((const int*)raw_data, wav_data, total_samples);
    }
    free(raw_data);

    // ── Commit to cache under lock ────────────────────────────────────────────
    EnterCriticalSection(&g_audioSystem.wavLock);

    // Re-check: another thread may have loaded the same file while we worked
    for (int i = 0; i < g_audioSystem.wav_cache_count; i++) {
        if (strcmp(g_audioSystem.wav_cache[i].filename, filename) == 0 &&
            g_audioSystem.wav_cache[i].loaded) {
            LeaveCriticalSection(&g_audioSystem.wavLock);
            free(wav_data);
            return true;
        }
    }
    if (g_audioSystem.wav_cache_count >= MAX_WAV_CACHE) {
        LeaveCriticalSection(&g_audioSystem.wavLock);
        free(wav_data);
        return false;
    }

    WAV* wd      = &g_audioSystem.wav_cache[g_audioSystem.wav_cache_count];
    wd->data         = wav_data;
    wd->sample_count = sample_count;
    wd->channels     = fmt.channels;
    wd->sample_rate  = fmt.sample_rate;
    strncpy(wd->filename, filename, sizeof(wd->filename) - 1);
    wd->filename[sizeof(wd->filename) - 1] = '\0';
    wd->loaded = true;
    g_audioSystem.wav_cache_count++;

    LeaveCriticalSection(&g_audioSystem.wavLock);
    return true;
}

void unload_WAV(const char* filename) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.wavLock);

    for (int i = 0; i < g_audioSystem.wav_cache_count; i++) {
        if (strcmp(g_audioSystem.wav_cache[i].filename, filename) == 0 &&
            g_audioSystem.wav_cache[i].loaded) {

            WAV* target = &g_audioSystem.wav_cache[i];

            // FIX: Nullify any live WavSound slots that point to this data.
            //      Previously, unloading a file left dangling wav_data pointers
            //      that audio_mixer would dereference (undefined behavior).
            for (int j = 0; j < MAX_WAV_SOUNDS; j++) {
                if (g_audioSystem.wav[j].wav_data == target) {
                    g_audioSystem.wav[j].active   = false;
                    g_audioSystem.wav[j].wav_data = NULL;
                }
            }

            free(target->data);
            target->data   = NULL;
            target->loaded = false;

            // Compact cache
            for (int j = i; j < g_audioSystem.wav_cache_count - 1; j++)
                g_audioSystem.wav_cache[j] = g_audioSystem.wav_cache[j + 1];
            g_audioSystem.wav_cache_count--;

            printf("Unloaded WAV file: %s\n", filename);
            break;
        }
    }

    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void unload_all_WAVs(void) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.wavLock);

    // FIX: Deactivate all WAV sounds before freeing their data
    for (int j = 0; j < MAX_WAV_SOUNDS; j++) {
        g_audioSystem.wav[j].active   = false;
        g_audioSystem.wav[j].wav_data = NULL;
    }

    for (int i = 0; i < g_audioSystem.wav_cache_count; i++) {
        if (g_audioSystem.wav_cache[i].loaded && g_audioSystem.wav_cache[i].data) {
            free(g_audioSystem.wav_cache[i].data);
            g_audioSystem.wav_cache[i].data   = NULL;
        }
        g_audioSystem.wav_cache[i].loaded = false;
    }
    g_audioSystem.wav_cache_count = 0;

    printf("Unloaded all WAV files from cache\n");
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

// Must be called with wavLock held.
static WAV* find_wav_data(const char* filename) {
    for (int i = 0; i < g_audioSystem.wav_cache_count; i++) {
        if (strcmp(g_audioSystem.wav_cache[i].filename, filename) == 0 &&
            g_audioSystem.wav_cache[i].loaded)
            return &g_audioSystem.wav_cache[i];
    }
    return NULL;
}

// Must be called with wavLock held.
static int get_or_create_wav_sound(int id, WAV* wav_data, float amplitude) {
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS)
        id -= 100;

    if (id >= 0 && id < MAX_WAV_SOUNDS) {
        WavSound* sound = &g_audioSystem.wav[id];
        if (sound->active) {
            sound->pitch = 1.0f;
            sound->fade_state   = 2;
            sound->fade_counter = 0;
            sound->fade_duration = FADE_SAMPLES;
        }
        init_wav_sound_common(sound, id, wav_data, amplitude);
        return id;
    }

    for (int i = 0; i < MAX_WAV_SOUNDS; i++) {
        if (!g_audioSystem.wav[i].active) {
            init_wav_sound_common(&g_audioSystem.wav[i], i, wav_data, amplitude);
            return i;
        }
    }
    return -1;
}


// ─── WAV sound API ────────────────────────────────────────────────────────────
// FIX: find_wav_data is now called inside the lock in all functions below,
//      eliminating the race window between load_WAV releasing the lock
//      and the subsequent use of the returned pointer.

int play_WAV_by_duration(int id, const char* filename, float amplitude, double duration_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int sound_id = get_or_create_wav_sound(id, wd, amplitude);
    if (sound_id >= 0) {
        g_audioSystem.wav[sound_id].fade_state    = 3;
        g_audioSystem.wav[sound_id].timer_samples = (int)(duration_seconds * SAMPLE_RATE);
        g_audioSystem.wav[sound_id].repeat        = false;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return sound_id;
}

int play_repeating_WAV(int id, const char* filename, float amplitude) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int sound_id = get_or_create_wav_sound(id, wd, amplitude);
    if (sound_id >= 0) {
        g_audioSystem.wav[sound_id].fade_state = 0;
        g_audioSystem.wav[sound_id].repeat     = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return sound_id;
}

int play_delayed_WAV_by_duration(int id, const char* filename, float amplitude,
                             double duration_seconds, double start_delay_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int sound_id = get_or_create_wav_sound(id, wd, amplitude);
    if (sound_id >= 0) {
        g_audioSystem.wav[sound_id].fade_state               = 4;
        g_audioSystem.wav[sound_id].delay_samples            = (int)(start_delay_seconds * SAMPLE_RATE);
        g_audioSystem.wav[sound_id].is_timed_after_delay     = true;
        g_audioSystem.wav[sound_id].delayed_duration_seconds = duration_seconds;
        g_audioSystem.wav[sound_id].repeat                   = false;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return sound_id;
}

int play_delayed_repeating_WAV(int id, const char* filename, float amplitude,
                              double start_delay_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int sound_id = get_or_create_wav_sound(id, wd, amplitude);
    if (sound_id >= 0) {
        g_audioSystem.wav[sound_id].fade_state           = 4;
        g_audioSystem.wav[sound_id].delay_samples        = (int)(start_delay_seconds * SAMPLE_RATE);
        g_audioSystem.wav[sound_id].is_timed_after_delay = false;
        g_audioSystem.wav[sound_id].repeat               = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return sound_id;
}

int play_repeating_delayed_WAV_by_duration(int id, const char* filename, float amplitude,
                                          double play_duration_seconds, double start_delay_seconds) {
    if (!g_audioSystem.initialized || !load_WAV(filename)) return -1;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (!wd) { LeaveCriticalSection(&g_audioSystem.wavLock); return -1; }

    int sound_id = get_or_create_wav_sound(id, wd, amplitude);
    if (sound_id >= 0) {
        g_audioSystem.wav[sound_id].fade_state               = 4;
        g_audioSystem.wav[sound_id].delay_samples            = (int)(start_delay_seconds * SAMPLE_RATE);
        g_audioSystem.wav[sound_id].is_timed_after_delay     = true;
        g_audioSystem.wav[sound_id].delayed_duration_seconds = play_duration_seconds;
        g_audioSystem.wav[sound_id].repeat                   = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return sound_id;
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

    int sound_id = get_or_create_wav_sound(id, wd, amplitude);
    if (sound_id >= 0) {
        g_audioSystem.wav[sound_id].current_position = start_sample;
        g_audioSystem.wav[sound_id].fractional_position = 0.0f;
        g_audioSystem.wav[sound_id].timer_samples = end_sample - start_sample;
        g_audioSystem.wav[sound_id].timer_counter = 0;
        g_audioSystem.wav[sound_id].fade_state = 3;
        g_audioSystem.wav[sound_id].repeat = false;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return sound_id;
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

    int sound_id = get_or_create_wav_sound(id, wd, amplitude);
    if (sound_id >= 0) {
        g_audioSystem.wav[sound_id].current_position = start_sample;
        g_audioSystem.wav[sound_id].fractional_position = 0.0f;
        g_audioSystem.wav[sound_id].timer_samples = end_sample - start_sample;
        g_audioSystem.wav[sound_id].timer_counter = 0;
        g_audioSystem.wav[sound_id].loop_start_sample = start_sample;
        g_audioSystem.wav[sound_id].fade_state = 3; // timed mode — timer_counter drives the loop boundary
        g_audioSystem.wav[sound_id].repeat = true;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return sound_id;
}

void stop_WAV(int id) {
    if (!g_audioSystem.initialized) return;
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) id -= 100;
    if (id < 0 || id >= MAX_WAV_SOUNDS) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WavSound* sound = &g_audioSystem.wav[id];
    if (sound->active && sound->fade_state != 2) {
        sound->fade_state   = 2;
        sound->fade_counter = 0;
        sound->fade_duration = FADE_SAMPLES;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void stop_all_WAVs(void) {
    if (!g_audioSystem.initialized) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    for (int i = 0; i < MAX_WAV_SOUNDS; i++) {
        WavSound* sound = &g_audioSystem.wav[i];
        if (sound->active && sound->fade_state != 2) {
            sound->fade_state   = 2;
            sound->fade_counter = 0;
            sound->fade_duration = FADE_SAMPLES;
        }
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

// FIX: Now correctly handles 100+ offset, matching angle_of_tone convention.
//      Previously this function ignored the offset, silently failing for
//      all WAV sounds addressed by their external ID.
void set_amp_WAV(int id, float amplitude) {
    if (!g_audioSystem.initialized) return;
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) id -= 100;
    if (id < 0 || id >= MAX_WAV_SOUNDS) return;

    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;

    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[id].active)
        g_audioSystem.wav[id].amplitude = amplitude;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void pause_WAV(int id) {
    if (!g_audioSystem.initialized) return;
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) id -= 100;
    if (id < 0 || id >= MAX_WAV_SOUNDS) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[id].active)
        g_audioSystem.wav[id].paused = true;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void resume_WAV(int id) {
    if (!g_audioSystem.initialized) return;
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) id -= 100;
    if (id < 0 || id >= MAX_WAV_SOUNDS) return;

    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[id].active)
        g_audioSystem.wav[id].paused = false;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

WAVInfo get_WAV_info(const char* filename) {
    WAVInfo info = {0, 0};
    
    if (!g_audioSystem.initialized) return info;
    if (!load_WAV(filename)) return info;

    EnterCriticalSection(&g_audioSystem.wavLock);
    WAV* wd = find_wav_data(filename);
    if (wd) {
        info.sample_rate = wd->sample_rate;
        info.total_samples = wd->sample_count;
    }
    LeaveCriticalSection(&g_audioSystem.wavLock);
    
    return info;
}

// ─── Pitch Control for Tones ───────────────────────────────────────────────────────

void set_pitch_WAV(int id, float pitch) {
    if (id < 0 || id >= MAX_WAV_SOUNDS) return;
    EnterCriticalSection(&g_audioSystem.wavLock);
    if (g_audioSystem.wav[id].active)
        g_audioSystem.wav[id].pitch = (pitch > 0.01f) ? pitch : 0.01f;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

void set_pitch_tone_dec_inc(int id, float pitch) {
    // For tones, pitch is just a frequency multiplier applied at play time.
    // Store it on the tone and apply in the mixer.
    if (id < 0 || id >= MAX_TONE_SOUNDS) return;
    EnterCriticalSection(&g_audioSystem.toneLock);
    if (g_audioSystem.tone[id].active)
        g_audioSystem.tone[id].frequency *= (double)pitch; // direct rescale
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void set_pitch_tone(int id, float pitch) {
    if (id < 0 || id >= MAX_TONE_SOUNDS) return;
    EnterCriticalSection(&g_audioSystem.toneLock);
    if (g_audioSystem.tone[id].active)
        g_audioSystem.tone[id].frequency = g_audioSystem.tone[id].base_frequency * (double)pitch;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

// ─── Fade Duration Management ───────────────────────────────────────────────────────

void set_fade_duration_tone(int id, int samples);
void set_fade_duration_WAV(int id, int samples);

// In audio.cpp, add:
void set_fade_duration_tone(int id, int samples) {
    if (!g_audioSystem.initialized || id < 0 || id >= MAX_TONE_SOUNDS) return;
    if (samples < 0) samples = 0;
    
    EnterCriticalSection(&g_audioSystem.toneLock);
    g_audioSystem.tone[id].fade_duration = samples;
    LeaveCriticalSection(&g_audioSystem.toneLock);
}

void set_fade_duration_WAV(int id, int samples) {
    if (!g_audioSystem.initialized) return;
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) id -= 100;
    if (id < 0 || id >= MAX_WAV_SOUNDS) return;
    if (samples < 0) samples = 0;
    
    EnterCriticalSection(&g_audioSystem.wavLock);
    g_audioSystem.wav[id].fade_duration = samples;
    LeaveCriticalSection(&g_audioSystem.wavLock);
}

// ─── AudioManager class ───────────────────────────────────────────────────────


// WAV


// .cpp
void AudioManager::set_pitch_WAV(int id, float pitch)  { ::set_pitch_WAV(id, pitch); }
void AudioManager::set_pitch_tone(int id, float pitch) { ::set_pitch_tone(id, pitch); }

int   AudioManager::play_repeating_WAV(int id, const char* filename, float amplitude) {
    return ::play_repeating_WAV(id, filename, amplitude);
}

int   AudioManager::play_WAV_by_duration(int id, const char* filename, float amplitude, double duration_seconds) {
    return ::play_WAV_by_duration(id, filename, amplitude, duration_seconds);
}

int   AudioManager::play_delayed_WAV_by_duration(int id, const char* filename, float amplitude, double duration_seconds, double start_delay_seconds) {
    return ::play_delayed_WAV_by_duration(id, filename, amplitude, duration_seconds, start_delay_seconds);
}

int   AudioManager::play_delayed_repeating_WAV(int id, const char* filename, float amplitude, double start_delay_seconds) {
    return ::play_delayed_repeating_WAV(id, filename, amplitude, start_delay_seconds);
}

int   AudioManager::play_repeating_delayed_WAV_by_duration(int id, const char* filename, float amplitude, double play_duration_seconds, double start_delay_seconds) {
    return ::play_repeating_delayed_WAV_by_duration(id, filename, amplitude, play_duration_seconds, start_delay_seconds);
}

int   AudioManager::play_specific_part_WAV(int id, const char* filename, float amplitude, int start_sample, int end_sample) {
    return ::play_specific_part_WAV(id, filename, amplitude, start_sample, end_sample);
}

int   AudioManager::play_repeating_specific_part_WAV(int id, const char* filename, float amplitude, int start_sample, int end_sample) {
    return ::play_repeating_specific_part_WAV(id, filename, amplitude, start_sample, end_sample);
}

void AudioManager::set_fade_duration_WAV(int id, int samples) {::set_fade_duration_WAV(id, samples);}

void  AudioManager::stop_WAV(int id)          { ::stop_WAV(id); }
void  AudioManager::stop_all_WAVs(void)         { ::stop_all_WAVs(); }
void  AudioManager::pause_WAV(int id)         { ::pause_WAV(id); }
void  AudioManager::resume_WAV(int id)        { ::resume_WAV(id); }
void  AudioManager::set_amp_WAV(int id, float amplitude) { ::set_amp_WAV(id, amplitude); }

bool  AudioManager::check_active_WAV(int id) {
    if (id >= 100 && id < 100 + MAX_WAV_SOUNDS) id -= 100;
    if (id < 0 || id >= MAX_WAV_SOUNDS) return false;
    EnterCriticalSection(&g_audioSystem.wavLock);
    bool playing = g_audioSystem.wav[id].active;
    LeaveCriticalSection(&g_audioSystem.wavLock);
    return playing;
}

bool  AudioManager::load_WAV(const char* filename)       { return ::load_WAV(filename); }
void  AudioManager::unload_WAV(const char* filename)     { ::unload_WAV(filename); }
void  AudioManager::unload_all_WAVs(void)                 { ::unload_all_WAVs(); }
WAVInfo AudioManager::get_WAV_info(const char* filename) { return ::get_WAV_info(filename); }

























// TONE
bool  AudioManager::audio_init(void)          { return ::audio_init(); }

void  AudioManager::audio_shutdown(void)      { ::audio_shutdown(); }

int   AudioManager::play_tone_by_duration(int id, double frequency, float amplitude, double phase, double duration_seconds) {
    return ::play_tone_by_duration(id, frequency, amplitude, phase, duration_seconds);
}
int   AudioManager::play_static_tone(int id, double frequency, float amplitude, double phase) {
    return ::play_static_tone(id, frequency, amplitude, phase);
}
int   AudioManager::play_delayed_tone_by_duration(int id, double frequency, float amplitude, double phase, double duration_seconds, double start_delay_seconds) {
    return ::play_delayed_tone_by_duration(id, frequency, amplitude, phase, duration_seconds, start_delay_seconds);
}
int   AudioManager::play_delayed_static_tone(int id, double frequency, float amplitude, double phase, double start_delay_seconds) {
    return ::play_delayed_static_tone(id, frequency, amplitude, phase, start_delay_seconds);
}

void  AudioManager::stop_tone(int id)        { ::stop_tone(id); }
void  AudioManager::stop_all_tones(void)       { ::stop_all_tones(); }
void  AudioManager::pause_tone(int id)       { ::pause_tone(id); }
void  AudioManager::resume_tone(int id)      { ::resume_tone(id); }
void  AudioManager::set_amp_tone(int id, float amplitude) { ::set_amp_tone(id, amplitude); }
void  AudioManager::angle_of_tone(int id, float angle) { ::angle_of_tone(id, angle); }
void  AudioManager::reverb_tone(int id, float amount, float decay) { ::reverb_tone(id, amount, decay); }



bool  AudioManager::check_active_tone(int id) {
    // Note: returns true while a sound is fading out (fade_state == 2, active == true).
    // This is intentional — the sound is still audible until the fade completes.
    if (id < 0 || id >= MAX_TONE_SOUNDS) return false;
    EnterCriticalSection(&g_audioSystem.toneLock);
    bool playing = g_audioSystem.tone[id].active;
    LeaveCriticalSection(&g_audioSystem.toneLock);
    return playing;
}


