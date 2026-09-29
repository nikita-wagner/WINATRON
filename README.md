# Super Development Engine

A small, header-only Windows engine written in **C (C17)** that talks to the OS through the **WinAPI only**. No SDL, no OpenAL, no third-party runtime: audio is WinMM, 2D graphics is GDI, timing is `QueryPerformanceCounter`, threads are `CreateThread`.

> Status: early development. Audio, clock, console, input and the GDI overlay work. Everything marked **Planned** below is a stub or an empty file waiting for implementation.

---

## Design rules

- **WinAPI only.** Every subsystem is built on `windows.h` and the system libraries (`user32`, `gdi32`, `winmm`, ...). This keeps the engine dependency-free and compatible with any Windows install.
- **SAL2 annotations** (`_In_`, `_In_opt_`, ...) on entry points for better static analysis and self-documenting parameters.
- **Header-only modules.** Each manager is a `.h` file included into a single translation unit (`source/main.c`).
- **Init / step / shutdown.** Every subsystem exposes the same three-part shape, and **never loops internally**:

  ```c
  if (!xxx_init()) return 1;          // acquire resources
  while (!shouldExit) { xxx_step(); } // ONE step per call (audio_pump, gdi_frame, ...)
  xxx_shutdown();                     // release in reverse order
  ```

  The loop, the pacing and the stop condition for every thread live together in [Threads.h](source/ThreadManager/Threads.h), so swapping a `Sleep()` for clock-based pacing touches one file.

---

## Project layout

```
source/
├── main.c                          wWinMain → MainThreads()
├── ThreadManager/Threads.h         thread procs, ThreadManager, shared exit flag
├── AudioManager/
│   ├── WINMM/winmm.h               waveOut mixer: tones + WAV playback
│   ├── WINMM/Piano.h               virtual piano (MIDI note → frequency)
│   └── WASAPI/wasapi.h             (planned, empty)
├── GraphicsManager/2D/
│   ├── GDI/GDI.h                   layered overlay + 32-bit ARGB software surface
│   └── Direct2D/Direct2D.h         (planned, empty)
├── ClockManager/Clock.h            QPC-based multi-clock frame pacing
├── ConsoleManager/Console.h        ANSI console, own printf-style formatter
├── InputManager/Keyboard.h         key held / pressed helpers, VK_ aliases
├── InputManager/Mouse.h            mouse button codes (minimal)
├── UserInterfaceManger/UI.h        UI clock + FPS readout
├── ScreenManager/Screen.h, Device.h   (planned, empty)
└── Testing/Testing.h               single-threaded test entry points
install.bat                         incremental build script
installer_config.txt                build configuration (generated on first run)
```

---

## Subsystems

### Threading: `ThreadManager/Threads.h`

- One shared `g_shouldExit` flag stops every managed thread at once.
- `tm_spawn()` is the only way to create a managed thread; `tm_shutdown()` is the only way out. It signals exit, waits, force-terminates stragglers after a timeout, and closes the handles.
- `MainThreads()` wraps the lifecycle in `__try / __finally`. That `__finally` plays the role a C++ destructor would, so shutdown runs even if startup fails partway.
- Current threads:

  | Thread  | State     | Job                                                   |
  |---------|-----------|-------------------------------------------------------|
  | Console | active    | owns the console, runs the UI clock                   |
  | Sound   | active    | `audio_init`, then `PlayPiano()` + `audio_pump()`     |
  | GDI     | available | written, currently commented out in `MainThreads`    |
  | Render  | stub      | empty proc, reserved for a future renderer            |

- Press **Esc** to shut the engine down.

### Audio: WinMM (`winmm.h`)

A software mixer on top of `waveOut`. No DirectSound, no XAudio.

- 44.1 kHz, 16-bit stereo, 3 ring buffers of 2205 frames (50 ms each).
- **Tones:** 100 slots, table-driven sine, fade in/out envelopes, timed and delayed starts, pitch, amplitude, pause/resume.
- **Auto-gain tones:** amplitude ramps across the note (2-step and 3-step curves).
- **Phase lock:** a tone's start phase is derived from the absolute sample clock, so repeated presses of the same note stack coherently instead of beating.
- **WAV playback:** 100 slots, a 32-file cache, linear-interpolation resampling, repeat, delayed start, sub-range playback, pitch shifting.
- **Spatial and effects:** stereo panning by angle (0 front, 90 right, 180 behind, 270 left) and a per-voice reverb delay line.
- **SIMD:** runtime CPUID detection (SSE2 / AVX2 / AVX-512) for sample-format conversion.
- **Threading:** `audio_init()` only opens the device. `audio_pump()` does one pass over the ring, and the Sound thread calls it. Mixing runs in place under a critical section, so `stop`/`pause`/`amp` calls are never lost.

Sound ID convention: tones `0..99`, WAVs `100..199`.

```c
play_tone_by_duration(id, 440.0, 0.5f, 0.0, 1.0);        // 1 s A4
play_tone_auto_gain_by_duration_3_steps(id, midi(69), 0.5, 0.0f, 0.2f, 0.0f);
angle_of_tone(id, 90.0f);                                // pan right
reverb_tone(id, 0.4f, 0.5f);
```

### Graphics: GDI (`GDI.h`)

A per-pixel-alpha, click-through, always-on-top overlay covering the whole virtual desktop.

- One `UINT32` per pixel, `0xAARRGGBB`, **premultiplied**, top-down, backed by a `CreateDIBSection`.
- Presented with `UpdateLayeredWindow` (`ULW_ALPHA`). `WS_EX_LAYERED | TRANSPARENT | TOPMOST | TOOLWINDOW | NOACTIVATE`.
- Multi-monitor safe: surface origin can be negative, and every pixel write is bounds-checked.
- `SetProcessDPIAware()` so pixels are real pixels.
- Provides `gdi_clear`, `gdi_draw_pixel`, `gdi_cursor`, `gdi_present`, and a demo frame (crosshair, outlined box, hue-cycling fill).
- **Thread affinity:** `gdi_init`, `gdi_frame` and `gdi_shutdown` must all run on the same thread, because a Win32 window belongs to the thread that created it. They refuse to run on a foreign thread. Other threads can stop it with `gdi_request_stop()` (posts `WM_QUIT`).

A larger primitive set (lines, rects, circles, alpha blending, GDI text) is drafted in a commented-out block at the bottom of the file, ready to be promoted.

### Clock: `Clock.h`

- Up to 16 named clocks (`MAX_CLOCKS`), backed by `QueryPerformanceCounter`, normalised to nanoseconds without 64-bit overflow.
- `clock_sync(id)` returns 1 when the next frame is due. It also maintains delta time (clamped after long stalls), rolling FPS (every ~0.25 s) and lifetime average FPS.
- API: `clock_create`, `clock_destroy`, `clock_set_fps`, `clock_reset`, `clock_get_fps / avg_fps / delta / uptime / frames`, `clock_find`, `clock_print_all`.

```c
UINT32 c = clock_create(60.0, "Game");
while (running) { if (clock_sync(c)) { update(clock_get_delta(c)); } }
```

### Console: `Console.h`

- Allocates or attaches a console and enables ANSI escape sequences.
- Own printf-style formatter (`con_printf`, `con_printf_color`) that writes straight to the console handle.
- Colour and style output, cursor movement/save/restore/hide, clear, resize query, title, box and line drawing, and a debug dump.

### Input: `Keyboard.h`, `Mouse.h`

- `input_key_held(vk)`, `input_key_pressed(vk)` (edge-triggered), `input_keys_held(count, ...)` for chords, and `input_print_keys()` for debugging.
- Underscore-suffixed key aliases (`VK_SPACE_`, `VK_A_`, ...). Mouse is currently just button codes.

### Piano: `Piano.h`

MIDI note to frequency with `f(n) = 440 · 2^((n-69)/12)`, and a per-key play loop. The key-to-tone bindings are commented out while the piano is being reworked.

---

## Building

Requirements: Windows and Visual Studio with the C++ (MSVC) toolchain. GCC and Clang are also supported by the script.

```bat
install.bat          :: 64-bit build  → bin64\, exe64\
install.bat 32bit    :: 32-bit build  → bin32\, exe32\
```

- On first run `install.bat` generates `installer_config.txt` and auto-detects `vcvars32.bat` / `vcvars64.bat` under `C:\Program Files\Microsoft Visual Studio\`.
- **Incremental:** each source file is MD5-hashed together with the metadata of the headers it includes (`/showIncludes`). Only changed files recompile. Any change to a tracked `.c/.cpp/.h/.hpp` file forces a full rebuild.
- When everything is up to date it **runs** `RUN_TARGET` (default `main.exe`) instead of rebuilding.
- Defaults: `/O2 /std:c17 /MT /W3`, `/SUBSYSTEM:WINDOWS`, `/DTESTING /DBEST`.
- Linked libraries: `user32 gdi32 shell32 advapi32 ole32 ws2_32 ntdll winmm`.
- `installer_config.txt` is git-ignored (`*.txt`), so each machine keeps its own paths.

---

## Roadmap / boilerplate in progress

These files or stubs exist so the structure is ready, but they are **not implemented yet**.

| Feature | Location | State |
|---|---|---|
| **WASAPI audio** (low-latency alternative to WinMM) | `AudioManager/WASAPI/wasapi.h` | empty file. Should follow the same `init / pump / shutdown` shape as `winmm.h` so the backend is swappable. |
| **Direct2D backend** | `GraphicsManager/2D/Direct2D/Direct2D.h` | empty file. Same lifecycle contract as GDI (init, clear, draw, present, destroy). |
| **Render thread** | `RenderThreadProc` in `Threads.h` | stub, returns immediately. Shape is documented in the comment above it. |
| **GDI thread** | `GDIThreadProc` in `Threads.h` | implemented, disabled in `MainThreads`. Uncomment the `tm_spawn` line to enable. |
| **GDI primitives** | bottom of `GDI.h` | drafted in a comment block: lines, rects, circles, alpha blend, text. |
| **Screen / Device manager** | `ScreenManager/Screen.h`, `Device.h` | empty files. Display and device enumeration. |
| **Mouse input** | `InputManager/Mouse.h` | button codes only. Needs held/pressed/position helpers to match the keyboard. |
| **Clock-paced threads** | `Sleep()` calls in `SoundThreadProc`, `GDIThreadProc` | marked `← Clock sync replaces this`. |
| **UI / FPS overlay** | `UserInterfaceManger/UI.h` | clock is created, `show_fps_con()` is commented out. |
| **Piano bindings** | `Piano.h` | key-to-tone mappings commented out. |

### Adding a new subsystem

1. Create `source/<Name>Manager/<name>.h` exposing `xxx_init()`, one-step `xxx_frame()` / `xxx_pump()` and `xxx_shutdown()`. No internal loops.
2. Add a thread proc in `Threads.h` following the pattern: init, then `while (!*shouldExit) { step; pace }`, then shutdown.
3. Register it in `MainThreads()` with `tm_spawn(&tm, Proc, param, "Name")`. Raise `MAX_THREADS` if needed.
4. If the subsystem owns a window, create, pump and destroy it on that one thread.

---

## Notes

- Compile flags define `TESTING` and `BEST`. `Testing/Testing.h` has single-threaded entry points such as `GDI_GRAPHICS_TEST()`, and the call in `main.c` is commented out.
- Header-only design means there is a single translation unit today. Keep non-`static` functions in headers in mind if a second `.c` file is ever added.
