#include <Windows.h>
#include "GraphicsManager/2D/GDI/GDI.h"
#include "InputManager/Keyboard.h"
#include "ConsoleManager/Console.h"
#include "ClockManager/Clock.h"
#include "AudioManager/Audio.h"
#include "AudioManager/Piano.h"
#include "Testing/Testing.h"




// Development points.
// Using only WinAPI functions for the compatibility with Windows operating systems
// Using SAL2 for better code analysis and documentation of function parameters







INT WINAPI wWinMain(
    _In_ HINSTANCE hInstance, 
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ PWSTR pCmdLine,
    _In_ int nCmdShow
) {


    INT test = Testing(hInstance);
    if (test) con_println_color(COL_RED,"test failed");

    return 0;
}
