// ═══════════════════════════════════════════════════════════════════════════
//  GDI.h — layered overlay window + 32-bit ARGB software surface
//
//  Threading contract — the same init / run / shutdown shape Audio.h uses:
//
//      gdi_init(hInstance)      acquire window, DCs and pixel buffer
//      gdi_frame()              one frame; FALSE once the window is gone
//      gdi_shutdown()           release everything gdi_init() acquired
//
//  ── All three MUST run on the SAME thread ────────────────────────────────
//  This is the one rule that makes GDI different from audio. Win32 windows
//  have thread affinity:
//
//    • messages for a window are delivered to the queue of the thread that
//      called CreateWindowEx — no other thread's PeekMessage will ever see
//      them, so a window created on main and pumped on a worker simply never
//      receives anything and Windows marks it "not responding";
//    • DestroyWindow only works when called from the owning thread;
//    • the DC and DIB section are bound to that window.
//
//  So GDIThreadProc calls all three itself rather than having main create the
//  window up front. That is the whole fix — see ThreadManager/Threads.h.
//  gdi_init() records its own thread id and the other entry points refuse to
//  run on a foreign thread instead of failing silently at runtime.
//
//  Stopping it from another thread: set *shouldExit (shared flag, same as
//  audio), or call gdi_request_stop() which posts WM_QUIT into the GDI
//  thread's queue — safe from any thread, unlike touching the window directly.
//
//  ── Pixel format ─────────────────────────────────────────────────────────
//  One UINT32 per pixel, 0xAARRGGBB, top-down rows. UpdateLayeredWindow with
//  AC_SRC_ALPHA needs PREMULTIPLIED colour (R,G,B already scaled by A).
// ═══════════════════════════════════════════════════════════════════════════

#include <Windows.h>
#include <stdbool.h>

#pragma once


// ─── Surface state ───────────────────────────────────────────────────────────
// One global, like g_audioSystem: the engine has a single overlay. Everything
// gdi_init() acquires is stored here so gdi_shutdown() can actually release it
// (the old code kept these in locals, so nothing could be freed).

typedef struct {
    HWND          hwnd;
    HDC           hdcScreen;    // screen DC — reference DC for the DIB, and present source
    HDC           hdcMem;       // memory DC the DIB is selected into
    HBITMAP       hbmp;         // the DIB section
    HBITMAP       hbmpOld;      // whatever was in hdcMem before, restored on shutdown
    UINT32*       pixels;       // direct pointer into the DIB: width*height premultiplied ARGB
    int           width;
    int           height;
    int           originX;      // surface (0,0) in virtual-desktop coords — negative on
    int           originY;      // multi-monitor setups where a screen sits left of / above primary
    DWORD         threadId;     // thread that owns the window; only it may pump or destroy
    BOOL          initialized;
    volatile BOOL running;
} GDISystem;

static GDISystem g_gdiSystem = {0};

#define GDI_FRAME_SLEEP_MS 1     // yield between frames instead of burning a core


// ─── Window procedure ────────────────────────────────────────────────────────
// The overlay is click-through (WS_EX_TRANSPARENT), so this only has to turn
// window teardown into a WM_QUIT for our own pump.

static LRESULT CALLBACK gdi_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}



// Test function to generate a cycling hue color for demonstration purposes.
// The `tick` state is not synchronised — only the GDI thread may call this.
static UINT32 RGB_HUE(void) {
    static FLOAT tick = 0;
    tick += 0.0002f;
    if (tick > 0x600) tick = 0;
    UINT32 segment = ((UINT32)tick / 256) % 6;
    UINT32 t = (UINT32)tick % 256;

    BYTE r, g, b;
    switch (segment) {
        case 0:  r = 255;             g = (BYTE)t;         b = 0;               break;
        case 1:  r = (BYTE)(255 - t); g = 255;             b = 0;               break;
        case 2:  r = 0;               g = 255;             b = (BYTE)t;         break;
        case 3:  r = 0;               g = (BYTE)(255 - t); b = 255;             break;
        case 4:  r = (BYTE)t;         g = 0;               b = 255;             break;
        default: r = 255;             g = 0;               b = (BYTE)(255 - t); break;
    }

    r = (BYTE)((r * 0x80) / 255);
    g = (BYTE)((g * 0x80) / 255);
    b = (BYTE)((b * 0x80) / 255);

    return ((UINT32)0x80 << 24) | ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
}

// ─── Lifecycle: init ─────────────────────────────────────────────────────────
// Call this ON the thread that will loop on gdi_frame(). It creates the window, so
// that thread becomes the owner of the message queue for the rest of the run.
//
// The surface covers the whole virtual desktop rather than a hardcoded
// 2560x1440: the old size was a guess, and any pixel write past the real
// desktop bounds corrupted heap memory behind the DIB.

BOOL gdi_init(_In_ HINSTANCE hInstance) {
    if (g_gdiSystem.initialized)
        return TRUE;

    ZeroMemory(&g_gdiSystem, sizeof(GDISystem));

    // Report real pixels instead of a DPI-scaled virtual resolution. Must
    // happen before the first window exists, or the overlay is stretched and
    // cursor coordinates stop lining up with the buffer.
    SetProcessDPIAware();

    g_gdiSystem.originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_gdiSystem.originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_gdiSystem.width   = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_gdiSystem.height  = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_gdiSystem.width <= 0 || g_gdiSystem.height <= 0)
        return FALSE;

    // Register once per process. Re-registering the same class name returns
    // ERROR_CLASS_ALREADY_EXISTS, which is fine — anything else is fatal.
    static const char* GDI_CLASS_NAME = "OverlayWindowClass";
    WNDCLASSA WindowClass  = {0};
    WindowClass.lpfnWndProc   = gdi_wnd_proc;
    WindowClass.hInstance     = hInstance;
    WindowClass.lpszClassName = GDI_CLASS_NAME;
    if (!RegisterClassA(&WindowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return FALSE;

    // WS_EX_LAYERED     per-pixel alpha via UpdateLayeredWindow
    // WS_EX_TRANSPARENT clicks pass through to whatever is underneath
    // WS_EX_TOPMOST     stays above normal windows
    // WS_EX_TOOLWINDOW  no taskbar button, no Alt-Tab entry
    // WS_EX_NOACTIVATE  never steals focus — important now that the overlay
    //                   lives on its own thread and could grab it at any time
    g_gdiSystem.hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        GDI_CLASS_NAME,
        "",
        WS_POPUP,
        g_gdiSystem.originX,
        g_gdiSystem.originY,
        g_gdiSystem.width,
        g_gdiSystem.height,
        NULL,
        NULL,
        hInstance,
        NULL
    );
    if (!g_gdiSystem.hwnd)
        return FALSE;

    g_gdiSystem.hdcScreen = GetDC(NULL);
    if (!g_gdiSystem.hdcScreen) { DestroyWindow(g_gdiSystem.hwnd); g_gdiSystem.hwnd = NULL; return FALSE; }

    g_gdiSystem.hdcMem = CreateCompatibleDC(g_gdiSystem.hdcScreen);
    if (!g_gdiSystem.hdcMem) {
        ReleaseDC(NULL, g_gdiSystem.hdcScreen);
        DestroyWindow(g_gdiSystem.hwnd);
        ZeroMemory(&g_gdiSystem, sizeof(GDISystem));
        return FALSE;
    }

    // Build a 32-bit ARGB DIB section
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = g_gdiSystem.width;
    bmi.bmiHeader.biHeight      = -g_gdiSystem.height;  // negative = top-down rows, y=0 at top
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;                   // 8 bits each for A, R, G, B
    bmi.bmiHeader.biCompression = BI_RGB;               // uncompressed: required for direct pixel writes

    g_gdiSystem.hbmp = CreateDIBSection(
        g_gdiSystem.hdcScreen,
        &bmi,
        DIB_RGB_COLORS,
        (void**)&g_gdiSystem.pixels,
        NULL,
        0
    );
    if (!g_gdiSystem.hbmp || !g_gdiSystem.pixels) {
        DeleteDC(g_gdiSystem.hdcMem);
        ReleaseDC(NULL, g_gdiSystem.hdcScreen);
        DestroyWindow(g_gdiSystem.hwnd);
        ZeroMemory(&g_gdiSystem, sizeof(GDISystem));
        return FALSE;
    }

    // Select the bitmap into the memory DC, keeping the old one for cleanup
    g_gdiSystem.hbmpOld = (HBITMAP)SelectObject(g_gdiSystem.hdcMem, g_gdiSystem.hbmp);
    SetBkMode(g_gdiSystem.hdcMem, TRANSPARENT);

    g_gdiSystem.threadId    = GetCurrentThreadId();   // the owner from here on
    g_gdiSystem.initialized = TRUE;
    g_gdiSystem.running     = TRUE;

    ShowWindow(g_gdiSystem.hwnd, SW_SHOWNOACTIVATE);
    return TRUE;
}


// ─── Frame helpers ───────────────────────────────────────────────────────────

// Wipe to fully transparent. Premultiplied transparent is all-zero bytes, so
// memset is the fast path.
static void gdi_clear(void) {
    if (!g_gdiSystem.pixels) return;
    memset(g_gdiSystem.pixels, 0,
           (size_t)g_gdiSystem.width * (size_t)g_gdiSystem.height * sizeof(UINT32));
}

static BOOL gdi_in_bounds(int x, int y) {
    return x >= 0 && y >= 0 && x < g_gdiSystem.width && y < g_gdiSystem.height;
}

// Write one pixel, ignoring out-of-bounds coordinates so callers never need
// their own clipping.
static void gdi_draw_pixel(int x, int y, UINT32 color) {
    if (!g_gdiSystem.pixels || !gdi_in_bounds(x, y)) return;
    g_gdiSystem.pixels[(size_t)y * (size_t)g_gdiSystem.width + (size_t)x] = color;
}

// Push the finished frame to the screen. The only call that touches the display.
static BOOL gdi_present(void) {
    if (!g_gdiSystem.hwnd) return FALSE;

    POINT ptSrc = {0, 0};                                          // top-left of our bitmap
    POINT ptDst = {g_gdiSystem.originX, g_gdiSystem.originY};      // where the window sits
    SIZE  size  = {g_gdiSystem.width, g_gdiSystem.height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};     // use the bitmap's own alpha

    return UpdateLayeredWindow(
        g_gdiSystem.hwnd, g_gdiSystem.hdcScreen, &ptDst, &size,
        g_gdiSystem.hdcMem, &ptSrc, 0, &blend, ULW_ALPHA
    );
}

// Cursor position converted to surface-local pixels. Raw screen coordinates
// are wrong the moment a second monitor sits left of or above the primary one
// (origin goes negative), which is how the old code wrote outside the buffer.
static void gdi_cursor(int* out_x, int* out_y) {
    POINT p = {0, 0};
    GetCursorPos(&p);
    if (out_x) *out_x = p.x - g_gdiSystem.originX;
    if (out_y) *out_y = p.y - g_gdiSystem.originY;
}

// Drain this thread's message queue. Returns FALSE once WM_QUIT arrives.
// A thread owning a window that never pumps is flagged "not responding" by
// Windows even while it is busy drawing.
static BOOL gdi_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return FALSE;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return TRUE;
}

// Ask the GDI thread to leave its frame loop. Safe to call from ANY thread —
// posting a message is the only legal way to reach a window you do not own.
void gdi_request_stop(void) {
    if (g_gdiSystem.threadId)
        PostThreadMessageA(g_gdiSystem.threadId, WM_QUIT, 0, 0);
}

// ─── Demo frame ──────────────────────────────────────────────────────────────
// Crosshair + outlined box + hue-cycling filled box around the cursor. Every
// write goes through gdi_draw_pixel(), which clips — the old version indexed
// the buffer directly with raw screen coordinates and wrote out of bounds
// whenever the cursor left the assumed 2560x1440 area.

static void gdi_draw_demo_frame(void) {
    int cx, cy;
    gdi_cursor(&cx, &cy);

    gdi_clear();

    // Full-surface crosshair through the cursor
    for (int x = 0; x < g_gdiSystem.width;  ++x) gdi_draw_pixel(x, cy, 0xFFFF0000);
    for (int y = 0; y < g_gdiSystem.height; ++y) gdi_draw_pixel(cx, y, 0xFFFF0000);

    // Outlined square around the cursor
    for (int x = -50; x <= 50; ++x) {
        gdi_draw_pixel(cx + x, cy - 50, 0x80FF0000);   // semi-transparent red
        gdi_draw_pixel(cx + x, cy + 50, 0x80FF0000);
    }
    for (int y = -50; y <= 50; ++y) {
        gdi_draw_pixel(cx - 50, cy + y, 0x80FF0000);
        gdi_draw_pixel(cx + 50, cy + y, 0x80FF0000);
    }

    // Filled square, cycling hue
    UINT32 hue = RGB_HUE();
    for (int y = -49; y <= 49; ++y) {
        for (int x = -49; x <= 49; ++x) gdi_draw_pixel(cx + x, cy + y, hue);
    }
}


// ─── Lifecycle: frame ────────────────────────────────────────────────────────
// ONE frame, mirroring audio_pump(): drain the message queue, draw, present.
// Returns FALSE when the window is gone (WM_QUIT from closing it or from
// gdi_request_stop()) and the caller should stop looping.
//
// The loop that calls this lives in GDIThreadProc (ThreadManager/Threads.h),
// so thread lifetime and pacing stay in one file. Pace it with
// GDI_FRAME_SLEEP_MS.
//
// Must run on the thread that called gdi_init(), because gdi_pump() only ever
// sees messages posted to its OWN thread queue. Called anywhere else it would
// spin forever on an empty queue while the real window stays frozen, so it
// refuses to run rather than doing that silently.

BOOL gdi_frame(void) {
    if (!g_gdiSystem.initialized) return FALSE;
    if (GetCurrentThreadId() != g_gdiSystem.threadId) return FALSE;
    if (!g_gdiSystem.running) return FALSE;

    if (!gdi_pump()) return FALSE;      // WM_QUIT — window closed or gdi_request_stop()

    gdi_draw_demo_frame();
    gdi_present();
    return TRUE;
}


// ─── Lifecycle: shutdown ─────────────────────────────────────────────────────
// Releases in reverse acquisition order. Like audio_shutdown(), the caller must
// guarantee the frame loop has already stopped; and like gdi_init(), it has to be
// the owning thread — DestroyWindow() is rejected outright when called from a
// thread that does not own the window.

void gdi_shutdown(void) {
    if (!g_gdiSystem.initialized) return;
    if (GetCurrentThreadId() != g_gdiSystem.threadId) return;

    g_gdiSystem.running = FALSE;

    if (g_gdiSystem.hdcMem && g_gdiSystem.hbmpOld) SelectObject(g_gdiSystem.hdcMem, g_gdiSystem.hbmpOld);
    if (g_gdiSystem.hbmp)      DeleteObject(g_gdiSystem.hbmp);
    if (g_gdiSystem.hdcMem)    DeleteDC(g_gdiSystem.hdcMem);
    if (g_gdiSystem.hdcScreen) ReleaseDC(NULL, g_gdiSystem.hdcScreen);
    if (g_gdiSystem.hwnd)      DestroyWindow(g_gdiSystem.hwnd);

    ZeroMemory(&g_gdiSystem, sizeof(GDISystem));
}


// ─── Single-threaded convenience ─────────────────────────────────────────────
// Init, loop and teardown on the caller's thread, with no exit flag — it runs
// until the window is closed. Used by Testing.h; the threaded path is
// GDIThreadProc in ThreadManager/Threads.h.

static VOID GDI_GRAPHICS_TEST(_In_ HINSTANCE hInstance) {
    if (!gdi_init(hInstance)) return;

    while (gdi_frame()) Sleep(GDI_FRAME_SLEEP_MS);

    gdi_shutdown();
}



























































































/*
// ═══════════════════════════════════════════════════════════════════════════════
// GDI.h — 2D software rasterizer backend (WinAPI GDI + layered overlay window)
//
// Renders into a 32-bit premultiplied ARGB DIB section and presents it with
// UpdateLayeredWindow, giving a per-pixel alpha, click-through, always-on-top
// overlay across the whole virtual desktop.
//
// Header-only on purpose: the build script compiles source\*.c non-recursively,
// so everything under 2D_GRAPHICS must be includable. All functions are static.
//
// ── Backend contract ──────────────────────────────────────────────────────────
// Every 2D backend in this folder implements the same five lifecycle calls, so
// a future Direct2D.h / D3D11.h can be swapped in without touching call sites:
//
//     init(surface, hInstance)   acquire device, window and pixel target
//     clear(surface)             wipe the frame to fully transparent
//     draw*(surface, ...)        rasterize primitives for this frame
//     refresh(surface)           present the finished frame to the screen
//     destroy(surface)           release everything init() acquired
//
// Only the draw* primitives are backend-specific in cost; the lifecycle names,
// argument order (surface first) and coordinate space (surface-local pixels,
// origin top-left) are fixed across backends.
//
// ── Pixel format ──────────────────────────────────────────────────────────────
// One UINT32 per pixel, 0xAARRGGBB, top-down rows, no padding.
// UpdateLayeredWindow with AC_SRC_ALPHA requires PREMULTIPLIED colour, i.e.
// R,G,B already scaled by A. Use gdi_rgba() to build colours correctly; the
// GDI_* constants below are opaque and therefore already premultiplied.
//
// Development points.
// Using only WinAPI functions for the compatibility with Windows operating systems
// Using SAL2 for better code analysis and documentation of function parameters
// ═══════════════════════════════════════════════════════════════════════════════

#ifndef ENGINE_2D_GRAPHICS_GDI_H
#define ENGINE_2D_GRAPHICS_GDI_H

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <wchar.h>


// ─── Surface ──────────────────────────────────────────────────────────────────
// One overlay window plus its CPU-side pixel target. Treat the fields as
// read-only from application code; use the functions below to mutate them.

typedef struct GDI_Surface {
    HWND    hwnd;        // Layered, click-through, topmost overlay window
    HDC     hdc_screen;  // Screen DC — reference DC for the DIB and present source
    HDC     hdc_mem;     // Memory DC the DIB is selected into (needed for text/GDI calls)
    HBITMAP hbmp;        // The DIB section itself
    HBITMAP hbmp_old;    // Bitmap that was in hdc_mem before us, restored on destroy
    UINT32* pixels;      // Direct pointer into the DIB — width*height premultiplied ARGB
    int     width;       // Surface width in pixels
    int     height;      // Surface height in pixels
    int     origin_x;    // Surface (0,0) in virtual-desktop coordinates, may be negative
    int     origin_y;    // Multi-monitor setups can place the desktop origin off-screen
} GDI_Surface;


// ─── Colour helpers ───────────────────────────────────────────────────────────

// Build a premultiplied ARGB pixel. a=255 leaves r,g,b untouched.
static UINT32 gdi_rgba(int r, int g, int b, int a) {
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    r = (r * a) / 255;
    g = (g * a) / 255;
    b = (b * a) / 255;
    return ((UINT32)a << 24) | ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
}

#define GDI_TRANSPARENT 0x00000000u
#define GDI_BLACK       0xFF000000u
#define GDI_WHITE       0xFFFFFFFFu
#define GDI_RED         0xFFFF0000u
#define GDI_GREEN       0xFF00FF00u
#define GDI_BLUE        0xFF0000FFu
#define GDI_YELLOW      0xFFFFFF00u
#define GDI_CYAN        0xFF00FFFFu
#define GDI_MAGENTA     0xFFFF00FFu
#define GDI_ORANGE      0xFFFF8000u

#define GDI_ALPHA_OF(c) (((c) >> 24) & 0xFFu)


// ─── Lifecycle: init ──────────────────────────────────────────────────────────

// Minimal window proc. The overlay never receives input (WS_EX_TRANSPARENT), so
// this only has to turn window teardown into a WM_QUIT for the message pump.
static LRESULT CALLBACK gdi_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Create the overlay window and its pixel target at an explicit position/size.
// Coordinates are virtual-desktop pixels. Returns 0 on failure, with the surface
// left safe to pass to gdi_destroy().
static int gdi_init_ex(GDI_Surface* s, HINSTANCE hinstance, int x, int y, int w, int h) {
    static int class_registered = 0;
    static const wchar_t* CLASS_NAME = L"Engine.GDI.Overlay";

    if (!s || w <= 0 || h <= 0) return 0;

    ZeroMemory(s, sizeof(*s));
    s->width    = w;
    s->height   = h;
    s->origin_x = x;
    s->origin_y = y;

    // Report real pixels instead of a scaled virtual resolution. Must happen
    // before the first window exists, otherwise the overlay is stretched and
    // cursor coordinates no longer line up with the buffer.
    SetProcessDPIAware();

    if (!class_registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc   = gdi_wnd_proc;
        wc.hInstance     = hinstance;
        wc.lpszClassName = CLASS_NAME;
        if (!RegisterClassW(&wc)) return 0;
        class_registered = 1;
    }

    // WS_EX_LAYERED     per-pixel alpha via UpdateLayeredWindow
    // WS_EX_TRANSPARENT clicks pass through to whatever is underneath
    // WS_EX_TOPMOST     stays above normal windows
    // WS_EX_TOOLWINDOW  no taskbar button, no Alt-Tab entry
    // WS_EX_NOACTIVATE  never steals focus from the app being overlaid
    s->hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLASS_NAME,
        L"",
        WS_POPUP,
        x, y, w, h,
        NULL, NULL, hinstance, NULL
    );
    if (!s->hwnd) return 0;

    s->hdc_screen = GetDC(NULL);
    if (!s->hdc_screen) return 0;

    s->hdc_mem = CreateCompatibleDC(s->hdc_screen);
    if (!s->hdc_mem) return 0;

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;      // Negative = top-down rows, y=0 at the top
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;      // 8 bits each for A, R, G, B
    bmi.bmiHeader.biCompression = BI_RGB;  // Uncompressed: required for direct pixel writes

    s->hbmp = CreateDIBSection(s->hdc_screen, &bmi, DIB_RGB_COLORS, (void**)&s->pixels, NULL, 0);
    if (!s->hbmp || !s->pixels) return 0;

    s->hbmp_old = (HBITMAP)SelectObject(s->hdc_mem, s->hbmp);

    // Text drawn later must not paint an opaque background box behind glyphs.
    SetBkMode(s->hdc_mem, TRANSPARENT);

    ShowWindow(s->hwnd, SW_SHOWNOACTIVATE);
    return 1;
}

// Create an overlay covering the entire virtual desktop (all monitors).
static int gdi_init(GDI_Surface* s, HINSTANCE hinstance) {
    return gdi_init_ex(
        s, hinstance,
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN)
    );
}


// ─── Lifecycle: clear / refresh / destroy ─────────────────────────────────────

// Wipe the frame to fully transparent. memset is the fast path because
// transparent is all-zero bytes in premultiplied ARGB.
static void gdi_clear(GDI_Surface* s) {
    if (!s || !s->pixels) return;
    memset(s->pixels, 0, (size_t)s->width * (size_t)s->height * sizeof(UINT32));
}

// Wipe the frame to a solid colour instead.
static void gdi_fill(GDI_Surface* s, UINT32 color) {
    if (!s || !s->pixels) return;
    if (color == 0) { gdi_clear(s); return; }
    size_t count = (size_t)s->width * (size_t)s->height;
    for (size_t i = 0; i < count; ++i) s->pixels[i] = color;
}

// Present the finished frame. This is the only call that touches the screen.
static int gdi_refresh(GDI_Surface* s) {
    if (!s || !s->hwnd) return 0;

    POINT pt_src = { 0, 0 };                        // Top-left of our bitmap
    POINT pt_dst = { s->origin_x, s->origin_y };    // Where the window sits on the desktop
    SIZE  size   = { s->width, s->height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };  // Use the bitmap's own alpha

    return UpdateLayeredWindow(
        s->hwnd, s->hdc_screen, &pt_dst, &size,
        s->hdc_mem, &pt_src, 0, &blend, ULW_ALPHA
    ) ? 1 : 0;
}

// Release everything init() acquired. Safe on a partially initialised surface,
// and safe to call twice.
static void gdi_destroy(GDI_Surface* s) {
    if (!s) return;
    if (s->hdc_mem && s->hbmp_old) SelectObject(s->hdc_mem, s->hbmp_old);
    if (s->hbmp)       DeleteObject(s->hbmp);
    if (s->hdc_mem)    DeleteDC(s->hdc_mem);
    if (s->hdc_screen) ReleaseDC(NULL, s->hdc_screen);
    if (s->hwnd)       DestroyWindow(s->hwnd);
    ZeroMemory(s, sizeof(*s));
}


// ─── Message pump ─────────────────────────────────────────────────────────────

// Drain every pending window message. A thread that never does this is marked
// "not responding" by Windows even while it is busy drawing — this is what
// keeps the overlay alive, not the drawing itself.
// Returns 0 once WM_QUIT arrives, so it can drive the frame loop directly:
//     while (gdi_pump()) { ...draw...; gdi_refresh(&surface); }
static int gdi_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return 0;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 1;
}


// ─── Coordinates ──────────────────────────────────────────────────────────────

// Translate a virtual-desktop point into surface-local pixels. Needed on
// multi-monitor setups where the desktop origin is not (0,0) — writing raw
// cursor coordinates into the buffer is an out-of-bounds write there.
static void gdi_to_surface(const GDI_Surface* s, int screen_x, int screen_y, int* out_x, int* out_y) {
    if (!s) return;
    if (out_x) *out_x = screen_x - s->origin_x;
    if (out_y) *out_y = screen_y - s->origin_y;
}

// Current cursor position, already converted to surface-local pixels.
static void gdi_cursor(const GDI_Surface* s, int* out_x, int* out_y) {
    POINT p = { 0, 0 };
    GetCursorPos(&p);
    gdi_to_surface(s, p.x, p.y, out_x, out_y);
}

static int gdi_in_bounds(const GDI_Surface* s, int x, int y) {
    return s && x >= 0 && y >= 0 && x < s->width && y < s->height;
}


// ─── Draw: pixels ─────────────────────────────────────────────────────────────

// Overwrite one pixel. Silently ignores out-of-bounds coordinates, so callers
// never need their own clipping.
static void gdi_draw_pixel(GDI_Surface* s, int x, int y, UINT32 color) {
    if (!gdi_in_bounds(s, x, y)) return;
    s->pixels[(size_t)y * (size_t)s->width + (size_t)x] = color;
}

// Source-over composite one premultiplied pixel onto what is already there.
// Use this instead of gdi_draw_pixel when the colour has alpha < 255.
static void gdi_blend_pixel(GDI_Surface* s, int x, int y, UINT32 color) {
    if (!gdi_in_bounds(s, x, y)) return;

    UINT32 sa = (color >> 24) & 0xFFu;
    if (sa == 0)   return;
    if (sa == 255) { gdi_draw_pixel(s, x, y, color); return; }

    UINT32* dst = &s->pixels[(size_t)y * (size_t)s->width + (size_t)x];
    UINT32  d   = *dst;
    UINT32  inv = 255u - sa;

    // dst = src + dst * (1 - src_alpha), per channel, both sides premultiplied.
    UINT32 a = sa                      + ((((d >> 24) & 0xFFu) * inv) / 255u);
    UINT32 r = ((color >> 16) & 0xFFu) + ((((d >> 16) & 0xFFu) * inv) / 255u);
    UINT32 g = ((color >>  8) & 0xFFu) + ((((d >>  8) & 0xFFu) * inv) / 255u);
    UINT32 b = ( color        & 0xFFu) + ((( d        & 0xFFu) * inv) / 255u);

    *dst = (a << 24) | (r << 16) | (g << 8) | b;
}

// Read a pixel back. Returns transparent for out-of-bounds reads.
static UINT32 gdi_get_pixel(const GDI_Surface* s, int x, int y) {
    if (!gdi_in_bounds(s, x, y)) return GDI_TRANSPARENT;
    return s->pixels[(size_t)y * (size_t)s->width + (size_t)x];
}


// ─── Draw: lines ──────────────────────────────────────────────────────────────

// Horizontal run, clipped. Writes a contiguous span, so this is the fastest
// primitive here and the one every fill routine is built on.
static void gdi_draw_hline(GDI_Surface* s, int x0, int x1, int y, UINT32 color) {
    if (!s || !s->pixels || y < 0 || y >= s->height) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= s->width) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= s->width) x1 = s->width - 1;

    UINT32* row = s->pixels + (size_t)y * (size_t)s->width;
    for (int x = x0; x <= x1; ++x) row[x] = color;
}

// Vertical run, clipped. Strided writes — one cache line per pixel.
static void gdi_draw_vline(GDI_Surface* s, int x, int y0, int y1, UINT32 color) {
    if (!s || !s->pixels || x < 0 || x >= s->width) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y1 < 0 || y0 >= s->height) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= s->height) y1 = s->height - 1;

    for (int y = y0; y <= y1; ++y) {
        s->pixels[(size_t)y * (size_t)s->width + (size_t)x] = color;
    }
}

// Arbitrary line, integer Bresenham — no division, no floats.
static void gdi_draw_line(GDI_Surface* s, int x0, int y0, int x1, int y1, UINT32 color) {
    if (!s || !s->pixels) return;
    if (y0 == y1) { gdi_draw_hline(s, x0, x1, y0, color); return; }
    if (x0 == x1) { gdi_draw_vline(s, x0, y0, y1, color); return; }

    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx >= 0 ? 1 : -1;
    int sy = dy >= 0 ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;

    for (;;) {
        gdi_draw_pixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// Full-surface crosshair through (x,y) — the canonical overlay primitive.
static void gdi_draw_crosshair(GDI_Surface* s, int x, int y, UINT32 color) {
    if (!s) return;
    gdi_draw_hline(s, 0, s->width  - 1, y, color);
    gdi_draw_vline(s, x, 0, s->height - 1, color);
}


// ─── Draw: rectangles ─────────────────────────────────────────────────────────

// Rectangle outline, 1px, inclusive of (x,y) and (x+w-1, y+h-1).
static void gdi_draw_rect(GDI_Surface* s, int x, int y, int w, int h, UINT32 color) {
    if (!s || w <= 0 || h <= 0) return;
    int x1 = x + w - 1, y1 = y + h - 1;
    gdi_draw_hline(s, x, x1, y,  color);
    gdi_draw_hline(s, x, x1, y1, color);
    gdi_draw_vline(s, x,  y, y1, color);
    gdi_draw_vline(s, x1, y, y1, color);
}

// Solid rectangle, clipped once up front instead of per pixel.
static void gdi_fill_rect(GDI_Surface* s, int x, int y, int w, int h, UINT32 color) {
    if (!s || !s->pixels || w <= 0 || h <= 0) return;

    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width)  x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x0 >= x1 || y0 >= y1) return;

    for (int py = y0; py < y1; ++py) {
        UINT32* row = s->pixels + (size_t)py * (size_t)s->width;
        for (int px = x0; px < x1; ++px) row[px] = color;
    }
}

// Translucency-aware rectangle fill. Slower than gdi_fill_rect — use it only
// when the colour actually has alpha < 255.
static void gdi_blend_rect(GDI_Surface* s, int x, int y, int w, int h, UINT32 color) {
    if (!s || w <= 0 || h <= 0) return;
    if (GDI_ALPHA_OF(color) == 255) { gdi_fill_rect(s, x, y, w, h, color); return; }
    for (int py = y; py < y + h; ++py) {
        for (int px = x; px < x + w; ++px) gdi_blend_pixel(s, px, py, color);
    }
}


// ─── Draw: circles ────────────────────────────────────────────────────────────

// Circle outline, integer midpoint algorithm, eight-way symmetry.
static void gdi_draw_circle(GDI_Surface* s, int cx, int cy, int radius, UINT32 color) {
    if (!s || radius < 0) return;
    int x = radius, y = 0, err = 1 - radius;

    while (x >= y) {
        gdi_draw_pixel(s, cx + x, cy + y, color);
        gdi_draw_pixel(s, cx + y, cy + x, color);
        gdi_draw_pixel(s, cx - y, cy + x, color);
        gdi_draw_pixel(s, cx - x, cy + y, color);
        gdi_draw_pixel(s, cx - x, cy - y, color);
        gdi_draw_pixel(s, cx - y, cy - x, color);
        gdi_draw_pixel(s, cx + y, cy - x, color);
        gdi_draw_pixel(s, cx + x, cy - y, color);

        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
}

// Solid disc, drawn as mirrored horizontal spans so each row is one contiguous
// write rather than a per-pixel distance test.
static void gdi_fill_circle(GDI_Surface* s, int cx, int cy, int radius, UINT32 color) {
    if (!s || radius < 0) return;
    int x = radius, y = 0, err = 1 - radius;

    while (x >= y) {
        gdi_draw_hline(s, cx - x, cx + x, cy + y, color);
        gdi_draw_hline(s, cx - x, cx + x, cy - y, color);
        gdi_draw_hline(s, cx - y, cx + y, cy + x, color);
        gdi_draw_hline(s, cx - y, cx + y, cy - x, color);

        ++y;
        if (err < 0) err += 2 * y + 1;
        else { --x; err += 2 * (y - x) + 1; }
    }
}


// ─── Draw: text ───────────────────────────────────────────────────────────────

// Draw a string using GDI's font rasterizer, then repair the alpha channel.
//
// GDI text routines write RGB but leave the alpha byte at 0, which
// UpdateLayeredWindow reads as fully transparent — text drawn naively is
// invisible. So we stamp alpha=255 onto every pixel inside the text box that
// GDI actually touched.
//
// Limitation of that trick: a glyph pixel whose RGB is exactly 0 cannot be told
// apart from untouched background, so text is always opaque and pure black text
// will not appear. Pass a non-black colour.
static void gdi_draw_text(GDI_Surface* s, int x, int y, const wchar_t* text, UINT32 color) {
    if (!s || !s->pixels || !s->hdc_mem || !text) return;

    int len = (int)wcslen(text);
    if (len <= 0) return;

    SIZE extent = { 0, 0 };
    GetTextExtentPoint32W(s->hdc_mem, text, len, &extent);

    SetTextColor(s->hdc_mem, RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
    TextOutW(s->hdc_mem, x, y, text, len);

    int x0 = x, y0 = y, x1 = x + extent.cx, y1 = y + extent.cy;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->width)  x1 = s->width;
    if (y1 > s->height) y1 = s->height;

    for (int py = y0; py < y1; ++py) {
        UINT32* row = s->pixels + (size_t)py * (size_t)s->width;
        for (int px = x0; px < x1; ++px) {
            if (row[px] & 0x00FFFFFFu) row[px] |= 0xFF000000u;
        }
    }
}


#endif // ENGINE_2D_GRAPHICS_GDI_H

*/