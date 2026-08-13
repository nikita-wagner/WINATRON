#include <Windows.h>
#include <stdbool.h>
#include "../AudioManager/Audio.h"
#include "../AudioManager/Piano.h"
#include "../ClockManager/Clock.h"
#include "../ConsoleManager/Console.h"
#include "../GraphicsManager/2D/GDI/GDI.h"

#pragma once
#define MAX_THREADS 8

// Shared exit flag — every managed thread proc polls this instead of owning
// its own stop signal, so one tm_shutdown() call can stop all of them at once.
static volatile bool g_shouldExit = false;

// ─── Thread procedures ───────────────────────────────────────────────────────
// Every thread has the same shape, and all of it is visible here:
//
//     init the subsystem  →  while (!*shouldExit) { one step; pace }  →  tear down
//
// The subsystem headers deliberately expose only the ONE-STEP call
// (audio_pump(), gdi_frame(), ...) and never loop internally, so the loop
// condition and the pacing of every thread live together in this file. Adding
// Clock sync means replacing a Sleep() in the loop below — nothing outside
// Threads.h has to change.



typedef struct {
    volatile bool* shouldExit;
    BOOL           new_con;     // TRUE = allocate our own console, FALSE = attach the parent's
} ConsoleThreadParams;

// Owns the console for as long as the thread lives. It has to keep running
// until *shouldExit rather than returning straight after init: MainThreads
// treats any finished thread as "time to shut down", so a proc that returns
// early would tear the whole engine down the moment it started.
DWORD WINAPI ConsoleThreadProc(LPVOID lpParam) {
    ConsoleThreadParams* p = (ConsoleThreadParams*)lpParam;

    if (p->new_con) {
        if (!new_con_init()) {
            MessageBoxA(NULL, "Failed to Attach New Console", "Error", MB_OK | MB_ICONERROR);
            return 1;
        }
    } else {
        if (!con_init()) {
            MessageBoxA(NULL, "Failed to Attach Old Console", "Error", MB_OK | MB_ICONERROR);
            return 2;
        }
    }

    while (!(p->shouldExit && *p->shouldExit)) {
        Sleep(16);
    }

    con_shutdown();


    return 0;
}






// Owns the audio system end to end. audio_init() only opens the device — the
// mixing loop is right here, so this thread is the only one that ever touches
// the waveOut ring.
DWORD WINAPI SoundThreadProc(LPVOID lpParam) {
    volatile bool* shouldExit = (volatile bool*)lpParam;

    if (!audio_init()) {
        MessageBoxA(NULL, "Failed to initialize audio system!", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // keep buffer refills punctual even when other threads are busy
    //SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // g_audioSystem.running also drops out of the loop if the device dies.
    while (g_audioSystem.running && !(shouldExit && *shouldExit)) {
        PlayPiano();
        audio_pump();
        Sleep(AUDIO_PUMP_INTERVAL_MS);   // ← Clock sync replaces this
    }

    audio_shutdown();


    return 0;
}





// A thread proc takes exactly one LPVOID, so anything needing more than the
// exit flag — GDI needs the HINSTANCE too — passes a struct instead. It must
// outlive the thread, so MainThreads keeps it on its own stack (which it does,
// because tm_shutdown() joins every thread before that stack frame goes away).
typedef struct {
    volatile bool* shouldExit;
    HINSTANCE      hInstance;
} GDIThreadParams;
// The whole GDI lifetime lives on THIS thread — create, pump, destroy.
// A Win32 window belongs to the thread that created it: only that thread's
// PeekMessage sees its messages and only that thread may DestroyWindow it.
// Creating the window on main and pumping it here would give a permanently
// frozen overlay, so gdi_init() is called here rather than up front.
DWORD WINAPI GDIThreadProc(LPVOID lpParam) {
    GDIThreadParams* p = (GDIThreadParams*)lpParam;

    if (!gdi_init(p->hInstance)) {
        MessageBoxA(NULL, "Failed to initialize GDI overlay!", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // gdi_frame() returns FALSE on WM_QUIT — the window was closed, or another
    // thread called gdi_request_stop().
    while (!(p->shouldExit && *p->shouldExit)) {
        if (!gdi_frame()) break;
        Sleep(GDI_FRAME_SLEEP_MS);       // ← Clock sync replaces this
    }

    gdi_shutdown();
    

    return 0;
}







// Stub. Same shape as the others when its system is ready:
//     if (!render_init()) return 1;
//     while (!(shouldExit && *shouldExit)) { render_frame(); Sleep(...); }
//     render_shutdown();
DWORD WINAPI RenderThreadProc(LPVOID lpParam) {
    volatile bool* shouldExit = (volatile bool*)lpParam;
    (void)shouldExit;
    return 0;
}





// ─── RAII-style thread lifecycle ─────────────────────────────────────────────
// C has no destructors, so nothing here is automatic the way a C++ RAII
// object would be. What we can guarantee natively on MSVC/Windows is a single
// paired teardown call: every HANDLE this manager creates lives only inside
// it, tm_spawn() is the only way in, and tm_shutdown() is the only way out —
// call it exactly once, from a __try/__finally block (see MainThread), so
// it still runs even if startup fails partway through or the function returns
// early. That __finally is doing the job a destructor would do in C++.

typedef struct {
    HANDLE      handle;
    DWORD       id;
    const char* name;
} ManagedThread;

typedef struct {
    ManagedThread threads[MAX_THREADS];
    int           count;
} ThreadManager;

// Creates `proc` as a new thread and registers it with `tm` so tm_shutdown()
// will wait for and close it. Returns FALSE (and shows an error box) if the
// manager is full or CreateThread fails.
static BOOL tm_spawn(ThreadManager* tm, LPTHREAD_START_ROUTINE proc, LPVOID param, const char* name) {
    if (tm->count >= MAX_THREADS) {
        MessageBoxA(NULL, "ThreadManager is full!", "Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    ManagedThread* slot = &tm->threads[tm->count];
    slot->handle = CreateThread(NULL, 0, proc, param, 0, &slot->id);
    if (!slot->handle) {
        char msg[128];
        wsprintfA(msg, "Failed to create thread \"%s\"!", name);
        MessageBoxA(NULL, msg, "Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    slot->name = name;
    tm->count++;
    return TRUE;
}





// Signals every managed thread to exit, waits for them, and force-terminates
// any straggler after `timeout_ms`. Safe to call once startup only got partway
// through (tm->count reflects only what actually got spawned).
static void tm_shutdown(ThreadManager* tm, DWORD timeout_ms) {
    if (tm->count == 0) return;

    g_shouldExit = true;

    HANDLE handles[MAX_THREADS];
    for (int i = 0; i < tm->count; i++) handles[i] = tm->threads[i].handle;

    if (WaitForMultipleObjects((DWORD)tm->count, handles, TRUE, timeout_ms) == WAIT_TIMEOUT) {
        for (int i = 0; i < tm->count; i++) {
            DWORD exitCode;
            if (GetExitCodeThread(handles[i], &exitCode) && exitCode == STILL_ACTIVE)
                TerminateThread(handles[i], 1);
        }
    }

    for (int i = 0; i < tm->count; i++) CloseHandle(handles[i]);
    tm->count = 0;
}





// Runs the Console and Sound threads under one ThreadManager until one of
// them exits or g_shouldExit is set from outside. Window/Render stay as
// unused stubs above until their systems are ready to join in.
int MainThreads(HINSTANCE hInstance) {

    ThreadManager tm = {0};
    g_shouldExit = false;

    // Per-thread parameters live here, on the frame that outlives every thread:
    // tm_shutdown() in the __finally joins them all before this returns, so the
    // threads can never read a dangling pointer.
    ConsoleThreadParams consoleParams = { &g_shouldExit, TRUE };
    GDIThreadParams     gdiParams     = { &g_shouldExit, hInstance };

    __try {

        if (!tm_spawn(&tm, ConsoleThreadProc, (LPVOID)&consoleParams, "Console")) return 2;
        if (!tm_spawn(&tm, SoundThreadProc,   (LPVOID)&g_shouldExit,  "Sound"))   return 3;
        if (!tm_spawn(&tm, GDIThreadProc,     (LPVOID)&gdiParams,     "GDI"))     return 4;


        HANDLE handles[MAX_THREADS];


        // Active Loop
        while (!g_shouldExit) {
            if(GetAsyncKeyState(VK_ESCAPE)) {
                g_shouldExit = true;
            } else {
                for (int i = 0; i < tm.count; i++) handles[i] = tm.threads[i].handle;
                DWORD result = WaitForMultipleObjects((DWORD)tm.count, handles, FALSE, 0); 

                if (result == WAIT_TIMEOUT) continue;
                break; // a thread finished, or WaitForMultipleObjects failed
            }
        }


        
    }
    __finally {

        tm_shutdown(&tm, 3000);


    }

    return 0;
}
