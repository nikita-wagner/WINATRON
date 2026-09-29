#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdarg.h>


// ═══════════════════════════════════════════════════════════════════════════════
// Console.h — Console output & cursor control (Win32 only, no CRT)
//
// Enables ANSI via SetConsoleMode, gets size via GetConsoleScreenBufferInfo,
// sets title via SetConsoleTitleA.
//
// All visible output goes through WriteConsoleA. Nothing here touches stdio:
// there is no stdout, no buffering and therefore no fflush. Formatting is done
// by con_vformat below rather than by the CRT's printf family.
// ═══════════════════════════════════════════════════════════════════════════════


// Foreground
#define COL_RESET        "\033[0m"
#define COL_BLACK        "\033[0;30m"
#define COL_RED          "\033[0;31m"
#define COL_GREEN        "\033[0;32m"
#define COL_YELLOW       "\033[0;33m"
#define COL_BLUE         "\033[0;34m"
#define COL_MAGENTA      "\033[0;35m"
#define COL_CYAN         "\033[0;36m"
#define COL_WHITE        "\033[0;37m"

// Bright foreground
#define COL_BR_BLACK     "\033[1;30m"
#define COL_BR_RED       "\033[1;31m"
#define COL_BR_GREEN     "\033[1;32m"
#define COL_BR_YELLOW    "\033[1;33m"
#define COL_BR_BLUE      "\033[1;34m"
#define COL_BR_MAGENTA   "\033[1;35m"
#define COL_BR_CYAN      "\033[1;36m"
#define COL_BR_WHITE     "\033[1;37m"

// Background
#define BG_BLACK         "\033[40m"
#define BG_RED           "\033[41m"
#define BG_GREEN         "\033[42m"
#define BG_YELLOW        "\033[43m"
#define BG_BLUE          "\033[44m"
#define BG_MAGENTA       "\033[45m"
#define BG_CYAN          "\033[46m"
#define BG_WHITE         "\033[47m"

// Styles
#define STY_BOLD         "\033[1m"
#define STY_DIM          "\033[2m"
#define STY_ITALIC       "\033[3m"
#define STY_UNDERLINE    "\033[4m"
#define STY_BLINK        "\033[5m"
#define STY_REVERSE      "\033[7m"
#define STY_STRIKE       "\033[9m"

#define CON_FMT_BUF 4096

// ─── Internal state ───────────────────────────────────────────────────────────

static int    g_ansi_enabled = 0;
static HANDLE g_con_out      = NULL;

// ─── Raw output ───────────────────────────────────────────────────────────────

static HANDLE con_handle(void) {
    if (g_con_out == NULL || g_con_out == INVALID_HANDLE_VALUE) {
        g_con_out = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    return g_con_out;
}

static void con_write(const CHAR* text, int len) {
    HANDLE h;
    DWORD  written;

    if (len <= 0) return;
    h = con_handle();
    if (h == NULL || h == INVALID_HANDLE_VALUE) return;

    if (!WriteConsoleA(h, text, (DWORD)len, &written, NULL)) {
        // WriteConsole only accepts real console handles. If output was
        // redirected to a file or pipe it fails, so fall back to WriteFile.
        WriteFile(h, text, (DWORD)len, &written, NULL);
    }
}

static void con_write_str(const CHAR* text) {
    if (text) con_write(text, lstrlenA(text));
}

static void con_write_repeat(CHAR ch, int count) {
    CHAR chunk[128];
    while (count > 0) {
        int n = (count < (int)sizeof(chunk)) ? count : (int)sizeof(chunk);
        for (int i = 0; i < n; i++) chunk[i] = ch;
        con_write(chunk, n);
        count -= n;
    }
}

// ─── Internal formatting ──────────────────────────────────────────────────────
// A small printf replacement, since dropping stdio also drops vsnprintf.
// Supported: %d %i %u %x %X %c %s %f %g %p %%, the flags '-', '0' and '+',
// a field width, a '.' precision and the l/ll/z length modifiers.
// %g differs from the CRT: it is fixed-point with trailing zeros trimmed,
// never exponential (except when the value is too large for the integer path).

typedef struct ConBuf {
    CHAR* data;
    int   cap;
    int   len;   // characters the format produced, even if they did not fit
} ConBuf;

typedef union ConF64 {
    double d;
    UINT64 u;
} ConF64;

static void cb_char(ConBuf* b, CHAR ch) {
    if (b->len < b->cap - 1) b->data[b->len] = ch;
    b->len++;
}

static void cb_str(ConBuf* b, const CHAR* s, int n) {
    for (int i = 0; i < n; i++) cb_char(b, s[i]);
}

static int u64_digits(UINT64 v, CHAR* out, UINT32 base, int upper) {
    const CHAR* lut = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    CHAR tmp[24];
    int  n = 0;

    if (v == 0) tmp[n++] = '0';
    while (v > 0) {
        tmp[n++] = lut[v % base];
        v /= base;
    }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

// Renders a non-negative double as <int>.<frac> with exactly prec decimals.
static int f_digits(double v, int prec, CHAR* out) {
    UINT64 scale = 1;
    UINT64 ip, fp;
    int    n;

    for (int i = 0; i < prec; i++) scale *= 10;

    ip = (UINT64)v;
    fp = (UINT64)((v - (double)ip) * (double)scale + 0.5);
    if (fp >= scale) {          // rounding carried into the integer part
        ip++;
        fp -= scale;
    }

    n = u64_digits(ip, out, 10, 0);
    if (prec > 0) {
        CHAR frac[24];
        int  fn = u64_digits(fp, frac, 10, 0);
        out[n++] = '.';
        for (int i = fn; i < prec; i++) out[n++] = '0';   // leading zeros
        for (int i = 0; i < fn; i++)    out[n++] = frac[i];
    }
    return n;
}

static void cb_padded(ConBuf* b, CHAR sign, const CHAR* body, int blen,
                      int width, int left, int zero) {
    int total = blen + (sign ? 1 : 0);
    int pad   = (width > total) ? width - total : 0;

    if (left) {
        if (sign) cb_char(b, sign);
        cb_str(b, body, blen);
        while (pad-- > 0) cb_char(b, ' ');
    } else if (zero) {
        if (sign) cb_char(b, sign);
        while (pad-- > 0) cb_char(b, '0');
        cb_str(b, body, blen);
    } else {
        while (pad-- > 0) cb_char(b, ' ');
        if (sign) cb_char(b, sign);
        cb_str(b, body, blen);
    }
}

static int con_vformat(CHAR* dst, int cap, const CHAR* fmt, va_list ap) {
    ConBuf b;
    b.data = dst;
    b.cap  = cap;
    b.len  = 0;

    if (cap <= 0) return 0;

    while (*fmt) {
        CHAR body[64];
        CHAR sign = 0;
        int  blen = 0;
        int  left = 0, zero = 0, plus = 0;
        int  width = 0, prec = -1, lng = 0;
        CHAR conv;

        if (*fmt != '%') {
            cb_char(&b, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            cb_char(&b, '%');
            fmt++;
            continue;
        }

        // flags
        for (;;) {
            if      (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { zero = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { fmt++; }
            else break;
        }

        // width
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            if (width < 0) { left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }

        // precision
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
            if (prec < 0) prec = 0;
        }

        // length modifier
        for (;;) {
            if      (*fmt == 'l') { lng++; fmt++; }
            else if (*fmt == 'h') { fmt++; }
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 2; fmt++; }
            else break;
        }
        if (lng > 2) lng = 2;

        conv = *fmt ? *fmt++ : 0;

        switch (conv) {
        case 'd':
        case 'i': {
            INT64  sv;
            UINT64 mag;
            if      (lng >= 2) sv = va_arg(ap, INT64);
            else if (lng == 1) sv = (INT64)va_arg(ap, long);
            else               sv = (INT64)va_arg(ap, int);

            if (sv < 0) {
                sign = '-';
                mag  = (UINT64)(-(sv + 1)) + 1;   // negates INT64_MIN safely
            } else {
                if (plus) sign = '+';
                mag = (UINT64)sv;
            }
            blen = u64_digits(mag, body, 10, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            UINT64 uv;
            if      (lng >= 2) uv = va_arg(ap, UINT64);
            else if (lng == 1) uv = (UINT64)va_arg(ap, unsigned long);
            else               uv = (UINT64)va_arg(ap, unsigned int);
            blen = u64_digits(uv, body, (conv == 'u') ? 10 : 16, conv == 'X');
            break;
        }
        case 'p': {
            UINT64 uv = (UINT64)(ULONG_PTR)va_arg(ap, void*);
            blen = u64_digits(uv, body, 16, 1);
            break;
        }
        case 'c': {
            body[0] = (CHAR)va_arg(ap, int);
            blen = 1;
            break;
        }
        case 's': {
            const CHAR* s = va_arg(ap, const CHAR*);
            int n, pad;
            if (!s) s = "(null)";
            n = lstrlenA(s);
            if (prec >= 0 && prec < n) n = prec;
            pad = (width > n) ? width - n : 0;
            // Emitted directly: a string may be longer than the body buffer.
            if (left) {
                cb_str(&b, s, n);
                while (pad-- > 0) cb_char(&b, ' ');
            } else {
                while (pad-- > 0) cb_char(&b, ' ');
                cb_str(&b, s, n);
            }
            continue;
        }
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            double dv = va_arg(ap, double);
            ConF64 bits;
            bits.d = dv;

            // Classify via the bit pattern: /fp:fast is free to fold away the
            // usual (v != v) NaN test, so it cannot be relied on here.
            if (((bits.u >> 52) & 0x7FF) == 0x7FF) {
                int is_nan = (bits.u & 0xFFFFFFFFFFFFFULL) != 0;
                if (!is_nan && (bits.u >> 63)) sign = '-';
                body[0] = is_nan ? 'n' : 'i';
                body[1] = is_nan ? 'a' : 'n';
                body[2] = is_nan ? 'n' : 'f';
                blen = 3;
                break;
            }

            if (prec < 0) prec = 6;
            if (prec > 17) prec = 17;

            if (bits.u >> 63) { sign = '-'; dv = -dv; }
            else if (plus)    { sign = '+'; }

            if (dv >= 1.0e18) {
                // Too large for the UINT64 integer path - use exponent form.
                int exp10 = 0;
                while (dv >= 1.0e18) { dv /= 10.0; exp10++; }
                blen = f_digits(dv, prec, body);
                body[blen++] = 'e';
                body[blen++] = '+';
                blen += u64_digits((UINT64)exp10, body + blen, 10, 0);
            } else {
                blen = f_digits(dv, prec, body);
                if (conv == 'g' || conv == 'G') {
                    while (blen > 0 && body[blen - 1] == '0') blen--;
                    if (blen > 0 && body[blen - 1] == '.') blen--;
                }
            }
            break;
        }
        default:
            cb_char(&b, '%');
            if (conv) cb_char(&b, conv);
            continue;
        }

        cb_padded(&b, sign, body, blen, width, left, zero);
    }

    dst[(b.len < cap) ? b.len : cap - 1] = '\0';
    return (b.len < cap) ? b.len : cap - 1;
}

// ─── con_init ─────────────────────────────────────────────────────────────────

static void con_enable_ansi(void) {
    // AllocConsole/AttachConsole replace the std handles, so re-fetch.
    g_con_out = GetStdHandle(STD_OUTPUT_HANDLE);

    // Enable Virtual Terminal Processing so ANSI codes work on Windows 10+
    if (g_con_out != INVALID_HANDLE_VALUE && g_con_out != NULL) {
        DWORD mode = 0;
        if (GetConsoleMode(g_con_out, &mode)) {
            if (SetConsoleMode(g_con_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                g_ansi_enabled = 1;
            }
        }
    }
}

static BOOL new_con_init(void) {
    BOOL nci = AllocConsole();
    con_enable_ansi();
    return nci;
}

static BOOL con_init(void) {
    BOOL ci = AttachConsole(ATTACH_PARENT_PROCESS);
    con_enable_ansi();
    return ci;
}

// ─── Output ───────────────────────────────────────────────────────────────────

static void con_print(const char* text) {
    con_write_str(text);
}

static void con_println(const char* text) {
    con_write_str(text);
    con_write("\n", 1);
}

static void con_printf(const char* fmt, ...) {
    CHAR    buf[CON_FMT_BUF];
    int     n;
    va_list args;

    va_start(args, fmt);
    n = con_vformat(buf, sizeof(buf), fmt, args);
    va_end(args);

    con_write(buf, n);
}

static void con_print_color(const char* color, const char* text) {
    if (g_ansi_enabled) {
        con_write_str(color);
        con_write_str(text);
        con_write_str(COL_RESET);
    } else {
        con_write_str(text);
    }
}

static void con_println_color(const char* color, const char* text) {
    con_print_color(color, text);
    con_write("\n", 1);
}

static void con_printf_color(const char* color, const char* fmt, ...) {
    CHAR    buf[CON_FMT_BUF];
    va_list args;

    va_start(args, fmt);
    con_vformat(buf, sizeof(buf), fmt, args);
    va_end(args);

    con_print_color(color, buf);
}

static void con_print_styled(const char* style, const char* color, const char* text) {
    if (g_ansi_enabled) {
        con_write_str(style);
        con_write_str(color);
        con_write_str(text);
        con_write_str(COL_RESET);
    } else {
        con_write_str(text);
    }
}

// ─── Screen ───────────────────────────────────────────────────────────────────

static void con_clear(void) {
    HANDLE h;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    COORD home = { 0, 0 };
    DWORD cells, written;

    if (g_ansi_enabled) {
        con_write_str("\033[2J\033[H");
        return;
    }

    // No ANSI: blank the screen buffer directly instead of shelling out to cls.
    h = con_handle();
    if (h == NULL || h == INVALID_HANDLE_VALUE) return;
    if (!GetConsoleScreenBufferInfo(h, &csbi)) return;

    cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    FillConsoleOutputCharacterA(h, ' ', cells, home, &written);
    FillConsoleOutputAttribute(h, csbi.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(h, home);
}

static void con_clear_line(void) {
    if (g_ansi_enabled) con_write_str("\033[2K");
}

// ─── Cursor ───────────────────────────────────────────────────────────────────

static void con_move(int row, int col) {
    if (g_ansi_enabled) {
        con_printf("\033[%d;%dH", row, col);
    } else {
        // ANSI is 1-based, the console API is 0-based.
        HANDLE h = con_handle();
        COORD  pos;
        pos.X = (SHORT)(col - 1);
        pos.Y = (SHORT)(row - 1);
        if (h != NULL && h != INVALID_HANDLE_VALUE) SetConsoleCursorPosition(h, pos);
    }
}

static void con_move_up(int n) {
    if (g_ansi_enabled && n > 0) con_printf("\033[%dA", n);
}

static void con_move_down(int n) {
    if (g_ansi_enabled && n > 0) con_printf("\033[%dB", n);
}

static void con_move_left(int n) {
    if (g_ansi_enabled && n > 0) con_printf("\033[%dD", n);
}

static void con_move_right(int n) {
    if (g_ansi_enabled && n > 0) con_printf("\033[%dC", n);
}

static void con_cursor_save(void) {
    if (g_ansi_enabled) con_write_str("\033[s");
}

static void con_cursor_restore(void) {
    if (g_ansi_enabled) con_write_str("\033[u");
}

static void con_cursor_show(void) {
    if (g_ansi_enabled) con_write_str("\033[?25h");
}

static void con_cursor_hide(void) {
    if (g_ansi_enabled) con_write_str("\033[?25l");
}

// ─── Terminal Info ────────────────────────────────────────────────────────────

static void con_get_size(int* width, int* height) {
    HANDLE h = con_handle();
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (h != NULL && h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &csbi)) {
        *width  = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        *height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        *width  = 80;
        *height = 25;
    }

}

static void con_set_title(const char* title) {
    SetConsoleTitleA(title);
}

static int con_ansi_enabled(void) {
    return g_ansi_enabled;
}

// ─── Drawing ──────────────────────────────────────────────────────────────────

static void con_draw_box(int x, int y, int w, int h, const char* color) {
    if (w < 2 || h < 1) return;

    // Top border
    con_move(y, x);
    con_print_color(color, "+");
    con_write_repeat('-', w - 2);
    con_print("+");

    // Side borders
    for (int i = 1; i < h - 1; i++) {
        con_move(y + i, x);
        con_print_color(color, "|");
        con_move(y + i, x + w - 1);
        con_print_color(color, "|");
    }

    // Bottom border
    if (h > 1) {
        con_move(y + h - 1, x);
        con_print_color(color, "+");
        con_write_repeat('-', w - 2);
        con_print("+");
    }

    con_write_str(COL_RESET);
}

static void con_draw_hline(int x, int y, int len, char ch) {
    con_move(y, x);
    con_write_repeat(ch, len);
}

static void con_draw_vline(int x, int y, int len, char ch) {
    for (int i = 0; i < len; i++) {
        con_move(y + i, x);
        con_write(&ch, 1);
    }
}

static  void con_fill(int x, int y, int w, int h, char ch, const char* color) {
    if (g_ansi_enabled) con_write_str(color);
    for (int row = 0; row < h; row++) {
        con_move(y + row, x);
        con_write_repeat(ch, w);
    }
    if (g_ansi_enabled) con_write_str(COL_RESET);
}

// ─── Debug overlay ────────────────────────────────────────────────────────────

static void con_debug(void) {
    int w, h;
    con_get_size(&w, &h);
    if (w < 4 || h < 4) return;

    // Hide cursor while drawing to avoid flicker
    con_cursor_hide();

    // ── Border (top + bottom rows, left + right columns) ─────────────────────
    con_write_str(COL_RED);

    // Top row
    con_move(1, 1);
    con_write_repeat('#', w);

    // Bottom row
    con_move(h, 1);
    con_write_repeat('#', w);

    // Left and right columns
    for (int r = 2; r < h; r++) {
        con_move(r, 1);
        con_write("#", 1);
        con_move(r, w);
        con_write("#", 1);
    }

    con_write_str(COL_RESET);

    // ── Resolution text at (row=2, col=2) ─────────────────────────────────────
    con_move(2, 2);
    con_printf(COL_BR_RED "[ %d x %d ]" COL_RESET, w, h);

    // ── Center dot ────────────────────────────────────────────────────────────
    int cx = w / 2;
    int cy = h / 2;
    con_move(cy, cx);
    con_write_str(COL_BR_WHITE "*" COL_RESET);

    con_cursor_show();
}


// ─── con_shutdown ─────────────────────────────────────────────────────────────

static void con_shutdown(void) {
    if (g_ansi_enabled) {
        con_write_str(COL_RESET);
        con_cursor_show();
    }
    FreeConsole();
    g_con_out = NULL;
}
