#include <Windows.h>

#pragma once

int width = 2560;
int height = 1440;


LRESULT CALLBACK wWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}



static _Out_ HWND CREATE_WINDOW(_In_ HINSTANCE hInstance) {
    // Register a window class for the overlay window
    // Register a window class for the overlay window
    WNDCLASS WindowClass = { 0 };


    WindowClass.lpfnWndProc = wWndProc;
    WindowClass.hInstance = hInstance;
    WindowClass.lpszClassName = "OverlayWindowClass";
    RegisterClass(&WindowClass);



    // Layered + transparent (click-through) + topmost + no taskbar icon
    HWND WindowHandle = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        WindowClass.lpszClassName, 
        "", 
        WS_POPUP,
        0, 
        0, 
        width, 
        height,
        NULL, 
        NULL, 
        hInstance, 
        NULL
    );

    return WindowHandle;
}



// Test function to generate a cycling hue color for demonstration purposes
_Out_ UINT32 RGB_HUE() {
    static FLOAT tick = 0;
    tick += 0.0002;
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




static VOID GDI_GRAPHICS_TEST(_In_ HINSTANCE hInstance) {
    
    HWND WindowHandle = CREATE_WINDOW(hInstance);

    // Build a 32-bit ARGB DIB section
    HDC hdcScreen = GetDC(NULL); // Get device context for the screen
    HDC hdcMem = CreateCompatibleDC(hdcScreen); // Create compatible memory device context for offscreen drawing

    BITMAPINFO bmi = { 0 }; // Initialize bitmap info structure
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); // Set size of bitmap header
    bmi.bmiHeader.biWidth = width; // Bitmap width in pixels
    bmi.bmiHeader.biHeight = -height; // Negative height = top-down bitmap (y=0 at top)
    bmi.bmiHeader.biPlanes = 1; // Always 1 for standard bitmaps
    bmi.bmiHeader.biBitCount = 32; // 32 bits per pixel (8 bits each for ARGB)
    bmi.bmiHeader.biCompression = BI_RGB; // No compression, raw RGB data for direct pixel access

    void* pixels; // Pointer to the pixel buffer (filled by CreateDIBSection)

    HBITMAP hbmp = CreateDIBSection( // Create DIB and get pointer to pixel data
        hdcScreen, 
        &bmi, 
        DIB_RGB_COLORS, 
        &pixels, 
        NULL, 
        0
    ); 

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbmp); // Select bitmap into memory DC, save the old one for cleanup

    // Push the bitmap into the layered window via UpdateLayeredWindow
    POINT ptSrc = { 0, 0 }; // Source coordinate in memory DC (top-left of bitmap)
    POINT ptDst = { 0, 0 }; // Destination coordinate on screen (window position)
    SIZE size = { width, height }; // Size of the area to copy
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA }; // Alpha blending: use source alpha, full opacity (255)

    MSG msg = {0};

    while (1) {
        // Non-blocking message check                   // Use PM_NOYIELD for better performance, but it can starve CPU resources.
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { // Use PM_REMOVE while Debugging to avoid starving CPU resources.
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            // Do your drawing here when no messages are pending
            POINT p;
            GetCursorPos(&p);
            memset(pixels, 0, width * height * 4);

            for (int x = 1; x < width; ++x) {
                ((UINT32*)pixels)[p.y * width + x] = 0xFFFF0000;
            }
            for (int y = 1; y < height; ++y) {
                ((UINT32*)pixels)[y * width + p.x] = 0xFFFF0000;
            }


            // Draw empty square around cursor
            for (int x = -50; x <= 50; ++x) {
                int px = p.x + x;
                int py1 = p.y - 50;
                int py2 = p.y + 50;
                if (px >= 0 && px < width) {
                    if (py1 >= 0 && py1 < height) ((UINT32*)pixels)[py1 * width + px] = 0x80FF0000; // Semi-transparent red
                    if (py2 >= 0 && py2 < height) ((UINT32*)pixels)[py2 * width + px] = 0x80FF0000; // Semi-transparent red
                }
            }
            for (int y = -50; y <= 50; ++y) {
                int py = p.y + y;
                int px1 = p.x - 50;
                int px2 = p.x + 50;
                if (py >= 0 && py < height) {
                    if (px1 >= 0 && px1 < width) ((UINT32*)pixels)[py * width + px1] = 0x80FF0000; // Semi-transparent red
                    if (px2 >= 0 && px2 < width) ((UINT32*)pixels)[py * width + px2] = 0x80FF0000; // Semi-transparent red
                }
            }
            // Draw filled square around cursor
            for (int y = -49; y <= 49; ++y) {
                int py = p.y + y;
                if (py >= 0 && py < height) {
                    for (int x = -49; x <= 49; ++x) {
                        int px = p.x + x;
                        if (px >= 0 && px < width) {
                            ((UINT32*)pixels)[py * width + px] = RGB_HUE(); // Semi-transparent red
                        }
                    }
                }
            }

            


            UpdateLayeredWindow(WindowHandle, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
            ShowWindow(WindowHandle, SW_SHOW);

            if (GetAsyncKeyState(VK_ESCAPE)) break;
        }
    }

    SelectObject(hdcMem, hOld);
    DeleteObject(hbmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    DestroyWindow(WindowHandle);
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