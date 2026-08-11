#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdarg.h>
#include <stdio.h>

// Test
// ─── Virtual Key Codes ────────────────────────────────────────────────────────

// Special Keys
#define VK_ESCAPE_       0x1B
#define VK_TAB_          0x09
#define VK_CAPSLOCK_     0x14
#define VK_SHIFT_        0x10
#define VK_CONTROL_      0x11
#define VK_ALT_          0x12
#define VK_SPACE_        0x20
#define VK_RETURN_       0x0D
#define VK_BACKSPACE_    0x08

// Arrow Keys
#define VK_LEFT_         0x25
#define VK_UP_           0x26
#define VK_RIGHT_        0x27
#define VK_DOWN_         0x28

// Function Keys
#define VK_F1_           0x70
#define VK_F2_           0x71
#define VK_F3_           0x72
#define VK_F4_           0x73
#define VK_F5_           0x74
#define VK_F6_           0x75
#define VK_F7_           0x76
#define VK_F8_           0x77
#define VK_F9_           0x78
#define VK_F10_          0x79
#define VK_F11_          0x7A
#define VK_F12_          0x7B

// Number Keys (top row)
#define VK_0_            0x30
#define VK_1_            0x31
#define VK_2_            0x32
#define VK_3_            0x33
#define VK_4_            0x34
#define VK_5_            0x35
#define VK_6_            0x36
#define VK_7_            0x37
#define VK_8_            0x38
#define VK_9_            0x39

// Alphabet Keys
#define VK_A_            0x41
#define VK_B_            0x42
#define VK_C_            0x43
#define VK_D_            0x44
#define VK_E_            0x45
#define VK_F_            0x46
#define VK_G_            0x47
#define VK_H_            0x48
#define VK_I_            0x49
#define VK_J_            0x4A
#define VK_K_            0x4B
#define VK_L_            0x4C
#define VK_M_            0x4D
#define VK_N_            0x4E
#define VK_O_            0x4F
#define VK_P_            0x50
#define VK_Q_            0x51
#define VK_R_            0x52
#define VK_S_            0x53
#define VK_T_            0x54
#define VK_U_            0x55
#define VK_V_            0x56
#define VK_W_            0x57
#define VK_X_            0x58
#define VK_Y_            0x59
#define VK_Z_            0x5A

// Numpad Keys
#define VK_NUMPAD0_      0x60
#define VK_NUMPAD1_      0x61
#define VK_NUMPAD2_      0x62
#define VK_NUMPAD3_      0x63
#define VK_NUMPAD4_      0x64
#define VK_NUMPAD5_      0x65
#define VK_NUMPAD6_      0x66
#define VK_NUMPAD7_      0x67
#define VK_NUMPAD8_      0x68
#define VK_NUMPAD9_      0x69
#define VK_MULTIPLY_     0x6A
#define VK_ADD_          0x6B
#define VK_SEPARATOR_    0x6C
#define VK_SUBTRACT_     0x6D
#define VK_DECIMAL_      0x6E
#define VK_DIVIDE_       0x6F

// Navigation Keys
#define VK_INSERT_       0x2D
#define VK_DELETE_       0x2E
#define VK_HOME_         0x24
#define VK_END_          0x23
#define VK_PAGEUP_       0x21
#define VK_PAGEDOWN_     0x22
#define VK_PRINTSCREEN_  0x2C
#define VK_PAUSE_        0x13
#define VK_SCROLLLOCK_   0x91


int input_key_held(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Is a specific key currently pressed (transition from up to down)?
BOOL input_key_pressed(int vk) {
    return (GetAsyncKeyState(vk) & 0x0001) != 0;
}

// Are multiple keys currently held down? (variadic function)
BOOL input_keys_held(int count, ...) {
    va_list args;
    va_start(args, count);
    for (int i = 0; i < count; i++) {
        int vk = va_arg(args, int);
        if (!input_key_held(vk)) {
            va_end(args);
            return FALSE;
        }
    }
    va_end(args);
    return TRUE;
}


void input_print_keys(void) {
    printf("Held keys:");
    // Check printable range + common special keys
    for (int vk = 0x08; vk <= 0xFF; vk++) {
        if (input_key_held(vk)) {
            // Try to print a readable name for common keys
            if (vk >= 0x41 && vk <= 0x5A)      printf(" %c", vk);        // A-Z
            else if (vk >= 0x30 && vk <= 0x39)  printf(" %c", vk);        // 0-9
            else if (vk == 0x20) printf(" SPACE");
            else if (vk == 0x0D) printf(" ENTER");
            else if (vk == 0x1B) printf(" ESC");
            else if (vk == 0x09) printf(" TAB");
            else if (vk == 0x10) printf(" SHIFT");
            else if (vk == 0x11) printf(" CTRL");
            else if (vk == 0x12) printf(" ALT");
            else if (vk == 0x08) printf(" BKSP");
            else if (vk == 0x25) printf(" LEFT");
            else if (vk == 0x26) printf(" UP");
            else if (vk == 0x27) printf(" RIGHT");
            else if (vk == 0x28) printf(" DOWN");
            else if (vk >= 0x70 && vk <= 0x7B) printf(" F%d", vk - 0x6F);
            else printf(" 0x%02X", vk);
        }
    }
    printf("\n");
}