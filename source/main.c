#include <Windows.h>
#include "GraphicsManager/2D/GDI/GDI.h"
#include "InputManager/KeyboardInput.h"
#include "ConsoleManager/Console.h"
#include "ClockManager/Clock.h"
// Development points.
// Using only WinAPI functions for the compatibility with Windows operating systems
// Using SAL2 for better code analysis and documentation of function parameters







INT WINAPI wWinMain(
    _In_ HINSTANCE hInstance, 
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ PWSTR pCmdLine,
    _In_ int nCmdShow
) {




    // Set process priority BEFORE your main loop
    // Option 1: LOW priority
    // SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
    // Option 2: BELOW NORMAL
    // SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    // Option 3: NORMAL (default)
    // SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    // Option 4: ABOVE NORMAL
    // SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    // Option 5: HIGH priority
    // SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    // Option 6: REALTIME (use with caution - can freeze system)
    // SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
    
    GDI_GRAPHICS_TEST(hInstance);
    


    new_con_init(); // Initialize console (enable ANSI codes, etc.)


    // clock_create returns the slot it used - sync that id, not a guessed one
    UINT32 timer0 = clock_create(1.0, "ClockTimer 0"); // Create a clock with 1 FPS target
    UINT32 timer1 = clock_create(2.0, "ClockTimer 1"); // Create a clock with 2 FPS target
    UINT32 timer2 = clock_create(4.0, "ClockTimer 2"); // Create a clock with 4 FPS target
    if (timer0 == CLOCK_INVALID) return 1;
    if (timer1 == CLOCK_INVALID) return 1;
    if (timer2 == CLOCK_INVALID) return 1;



    while (1) { // Main loop while the clock is active

        if (clock_sync(timer0)) con_println_color(COL_RED, "Tick 0.");
        if (clock_sync(timer1)) con_println_color(COL_GREEN, "Tick 1.");
        if (clock_sync(timer2)) con_println_color(COL_BLUE, "Tick 2.");

        

        if (input_keys_held(2, VK_CONTROL_, VK_SHIFT_)) {
            con_println_color(COL_YELLOW, "Control + Shift are held down.");

        }

        if (input_key_pressed(VK_ESCAPE_)) {
            break; // Exit the loop if Escape key is pressed
        }
    }
    
    clock_destroy_all(); // Clean up all clocks
    con_println_color(COL_CYAN, "Exiting program.");
    con_shutdown(); // Clean up console (reset colors, show cursor, etc.)



    Sleep(1000); // Optional: wait a moment before closing to see the final message







    return 0;
}