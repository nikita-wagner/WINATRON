#include <Windows.h>
#include "ThreadManager/Threads.h"



// Development points.
// Using only WinAPI functions for the compatibility with Windows operating systems
// Using SAL2 for better code analysis and documentation of function parameters







INT WINAPI wWinMain(
    _In_ HINSTANCE hInstance, 
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ PWSTR pCmdLine,
    _In_ int nCmdShow
) {


    // INT test = Testing(hInstance);
    // if (test) con_println_color(COL_RED,"test failed");

    MainThreads(hInstance);

    return 0;
}
