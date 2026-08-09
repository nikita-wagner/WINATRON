#pragma once

#include <windows.h>
#include "../ConsoleManager/Console.h"

#define MAX_CLOCKS 16

// Returned by clock_create/clock_find when no slot matches. clock_id is
// unsigned, so a plain -1 would compare as a huge valid-looking number.
#define CLOCK_INVALID ((UINT32)-1)

// ─── Clock slot ───────────────────────────────────────────────────────────────
// Time values stored as UINT64 nanoseconds from an arbitrary monotonic epoch.

typedef struct Clock {
    UINT64      start_time;          // ns — when this clock was created
    UINT64      last_frame_time;     // ns — timestamp of last SyncClock success
    UINT64      last_fps_update;     // ns — when we last computed current_fps
    double      target_fps;          // desired frames per second
    double      target_duration;     // 1.0 / target_fps (seconds)
    double      current_fps;         // rolling FPS (updated every ~0.25s)
    double      average_fps;         // lifetime average
    double      delta_time;          // seconds between last two frames
    UINT64      total_frames;        // lifetime frame count
    UINT32      recent_frame_count;  // frames since last fps update
    CHAR        name[32];            // human-readable label
    BOOL        active;              // 1 = in use, 0 = free slot
} Clock;

// ═══════════════════════════════════════════════════════════════════════════════
// Clock.h — High-precision clock system (C)
//
// Backend: QueryPerformanceCounter — monotonic and high-resolution, and unlike
// <chrono> it is plain C. Raw counter ticks are normalised to nanoseconds so
// every clock slot stores the same unit regardless of the machine's frequency.
// ═══════════════════════════════════════════════════════════════════════════════



// ─── Internal time helpers ────────────────────────────────────────────────────

static UINT64 now_ns(void) {
    static LARGE_INTEGER freq;   // zero-initialised; QPF is constant after boot
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&counter);

    // Split into whole seconds + remainder. Multiplying the raw counter by
    // 1000000000 directly would overflow 64 bits after a few hours of uptime.
    return (UINT64)(counter.QuadPart / freq.QuadPart) * 1000000000ULL
         + (UINT64)(counter.QuadPart % freq.QuadPart) * 1000000000ULL / (UINT64)freq.QuadPart;
}

static double ns_to_sec(UINT64 ns) {
    return (double)ns / 1000000000.0;
}

// ─── Global clock array ──────────────────────────────────────────────────────

static Clock g_clocks[MAX_CLOCKS];

static UINT32 valid(UINT32 id) {
    return (id < MAX_CLOCKS && g_clocks[id].active);
}

// ─── clock_create ─────────────────────────────────────────────────────────────

UINT32 clock_create(double fps, const CHAR* name) {
    for (UINT32 i = 0; i < MAX_CLOCKS; i++) {
        if (!g_clocks[i].active) {
            Clock* c = &g_clocks[i];
            ZeroMemory(c, sizeof(Clock));

            if (fps <= 0.0) fps = 60.0;
            c->target_fps      = fps;
            c->target_duration = 1.0 / fps;
            c->start_time      = now_ns();
            c->last_fps_update = c->start_time;
            c->last_frame_time = 0;  // signals "first frame"
            c->active          = 1;

            // lstrcpynA copies at most n-1 chars and always NUL-terminates
            lstrcpynA(c->name, name ? name : "unnamed", sizeof(c->name));

            return i;
        }
    }
    con_printf("[Clock] No free slots (max %d)\n", MAX_CLOCKS);
    return CLOCK_INVALID;
}

// ─── clock_destroy ────────────────────────────────────────────────────────────

VOID clock_destroy(UINT32 clock_id) {
    if (clock_id >= MAX_CLOCKS) return;
    ZeroMemory(&g_clocks[clock_id], sizeof(Clock));
}

VOID clock_destroy_all(void) {
    for (UINT32 i = 0; i < MAX_CLOCKS; i++) {
        if (g_clocks[i].active) clock_destroy(i);
    }
}

// ─── clock_sync ───────────────────────────────────────────────────────────────
// The heart of the system. Returns 1 when enough time has passed for the
// next frame. Computes delta, updates FPS counters.

UINT32 clock_sync(UINT32 clock_id) {
    if (!valid(clock_id)) return 0;

    Clock* c = &g_clocks[clock_id];
    UINT64 current = now_ns();

    // First frame — always trigger
    if (c->last_frame_time == 0) {
        c->last_frame_time = current;
        c->delta_time      = 0.0;
        c->total_frames    = 1;
        c->recent_frame_count = 1;
        return 1;
    }

    double elapsed = ns_to_sec(current - c->last_frame_time);

    // Not time yet
    if (elapsed < c->target_duration) {
        return 0;
    }

    // Compute delta time
    c->delta_time = elapsed;

    // Clamp large gaps (e.g. debugger pause) to avoid physics explosions
    if (c->delta_time > c->target_duration * 3.0) {
        c->delta_time = c->target_duration;
    }

    c->last_frame_time = current;
    c->total_frames++;
    c->recent_frame_count++;

    // ── Update rolling FPS (every ~0.25s) ─────────────────────────────────
    double fps_elapsed = ns_to_sec(current - c->last_fps_update);
    if (fps_elapsed >= 0.25) {
        c->current_fps        = (double)c->recent_frame_count / fps_elapsed;
        c->recent_frame_count = 0;
        c->last_fps_update    = current;
    }

    // ── Update lifetime average FPS ───────────────────────────────────────
    double total_elapsed = ns_to_sec(current - c->start_time);
    if (total_elapsed > 0.0) {
        c->average_fps = (double)c->total_frames / total_elapsed;
    }

    return 1;
}

// ─── clock_set_fps ────────────────────────────────────────────────────────────

VOID clock_set_fps(UINT32 clock_id, double fps) {
    if (!valid(clock_id)) return;
    if (fps <= 0.0) fps = 60.0;
    g_clocks[clock_id].target_fps      = fps;
    g_clocks[clock_id].target_duration = 1.0 / fps;
}

// ─── clock_reset ──────────────────────────────────────────────────────────────

VOID clock_reset(UINT32 clock_id) {
    if (!valid(clock_id)) return;
    Clock* c = &g_clocks[clock_id];
    UINT64 now = now_ns();
    c->start_time         = now;
    c->last_fps_update    = now;
    c->last_frame_time    = 0;
    c->total_frames       = 0;
    c->recent_frame_count = 0;
    c->current_fps        = 0.0;
    c->average_fps        = 0.0;
    c->delta_time         = 0.0;
}

// ─── Queries ──────────────────────────────────────────────────────────────────

double clock_get_fps(UINT32 clock_id) {
    if (!valid(clock_id)) return 0.0;
    return g_clocks[clock_id].current_fps;
}

double clock_get_avg_fps(UINT32 clock_id) {
    if (!valid(clock_id)) return 0.0;
    return g_clocks[clock_id].average_fps;
}

double clock_get_target_fps(UINT32 clock_id) {
    if (!valid(clock_id)) return 0.0;
    return g_clocks[clock_id].target_fps;
}

double clock_get_delta(UINT32 clock_id) {
    if (!valid(clock_id)) return 0.0;
    return g_clocks[clock_id].delta_time;
}

double clock_get_uptime(UINT32 clock_id) {
    if (!valid(clock_id)) return 0.0;
    if (g_clocks[clock_id].start_time == 0) return 0.0;
    return ns_to_sec(now_ns() - g_clocks[clock_id].start_time);
}

UINT64 clock_get_frames(UINT32 clock_id) {
    if (!valid(clock_id)) return 0;
    return g_clocks[clock_id].total_frames;
}

const CHAR* clock_get_name(UINT32 clock_id) {
    if (!valid(clock_id)) return "invalid";
    return g_clocks[clock_id].name;
}

UINT32 clock_is_active(UINT32 clock_id) {
    if (clock_id >= MAX_CLOCKS) return 0;
    return g_clocks[clock_id].active;
}

// ─── Utility ──────────────────────────────────────────────────────────────────

double clock_now(void) {
    return ns_to_sec(now_ns());
}

UINT32 clock_find(const CHAR* name) {
    if (!name) return CLOCK_INVALID;
    for (UINT32 i = 0; i < MAX_CLOCKS; i++) {
        if (g_clocks[i].active && lstrcmpA(g_clocks[i].name, name) == 0)
            return i;
    }
    return CLOCK_INVALID;
}

UINT32 clock_count(void) {
    UINT32 n = 0;
    for (UINT32 i = 0; i < MAX_CLOCKS; i++) {
        if (g_clocks[i].active) n++;
    }
    return n;
}

// ─── Debug ────────────────────────────────────────────────────────────────────

VOID clock_print(UINT32 clock_id) {
    if (!valid(clock_id)) {
        con_printf("[Clock %u] invalid or inactive\n", clock_id);
        return;
    }
    Clock* c = &g_clocks[clock_id];
    con_printf("[Clock %u] \"%s\"\n", clock_id, c->name);
    con_printf("  Target:  %.4g FPS (%.4f ms/frame)\n", c->target_fps, c->target_duration * 1000.0);
    con_printf("  Current: %.1f FPS\n", c->current_fps);
    con_printf("  Average: %.1f FPS\n", c->average_fps);
    con_printf("  Delta:   %.4f ms\n", c->delta_time * 1000.0);
    con_printf("  Frames:  %llu\n", (unsigned long long)c->total_frames);
    con_printf("  Uptime:  %.2f s\n", clock_get_uptime(clock_id));
}

static VOID clock_print_all(void) {
    con_printf("\n=== Active Clocks (%u / %d) ===\n", clock_count(), MAX_CLOCKS);
    for (UINT32 i = 0; i < MAX_CLOCKS; i++) {
        if (g_clocks[i].active) {
            Clock* c = &g_clocks[i];
            con_printf("  [%2u] %-12s | target %6.1f | current %6.1f | avg %6.1f | %8llu frames | %.1fs\n",
                i, c->name, c->target_fps, c->current_fps, c->average_fps,
                (unsigned long long)c->total_frames, clock_get_uptime(i));
        }
    }
    con_printf("==============================\n\n");
}
