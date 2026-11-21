# toggleHDR — native tray mode

This branch extends [Clayton Macleod's toggleHDR](https://github.com/ShortyCM/toggleHDR) with an optional native Windows tray application. It is a C++ executable and does not need AutoHotkey, Python, .NET, or a separately installed C++ runtime.

The original behavior remains available: run `toggleHDR.exe` to toggle HDR, refresh Windows Display Settings, then exit. Run `toggleHDR.exe --tray` to keep a status icon and shortcut handler running.

## Tray behavior

- The icon shows **HDR** or **SDR** in smooth, condensed Bahnschrift lettering on a transparent background. It has no box or border. The monochrome text changes between white and black to match the Windows taskbar theme.
- The status comes from the selected display's actual mode. SDR with wide-color management still appears as **SDR**. A question mark indicates that the display or its state is unavailable.
- A lightweight state check follows changes made outside the application. The icon is replaced only when the state or theme changes.
- **Win+Alt+B** keeps Windows' native HDR toggle and refreshes the selected display's profile afterward. The native Windows/Game Bar shortcut must be available. The tray menu can toggle the selected display directly.
- After a toggle, the Display Settings workaround refreshes the active color profile and closes the Settings window. An optional SDR profile selection can explicitly restore a monitor's SDR calibration.

Only one resident instance is needed. Use `--quit` to stop it.

## Usage

```text
toggleHDR.exe
toggleHDR.exe --tray
toggleHDR.exe --tray --display "\\.\DISPLAY1"
toggleHDR.exe --status
toggleHDR.exe --on
toggleHDR.exe --off
toggleHDR.exe --refresh
toggleHDR.exe --quit
toggleHDR.exe --help
```

| Option | Purpose |
| --- | --- |
| `--tray` | Start the resident tray application. |
| `--display NAME` | Select a connected display by its Windows display name or monitor friendly name. Without this option, prefer an HDR-capable primary display, otherwise the first HDR-capable display. |
| `--status` | Print display status as JSON without changing the display. |
| `--on`, `--off` | Explicitly enable or disable HDR. |
| `--refresh` | Refresh the color profile without changing HDR mode. |
| `--no-icc` | Disable all color-profile refresh work. Cannot be combined with `--sdr-profile`. |
| `--sdr-profile FILE` | Select this already-associated SDR profile after returning to SDR. The filename must refer to an installed profile for the selected display. |
| `--sdr-reset-profile FILE` | Before restoring the desired SDR profile, briefly select this other already-associated SDR profile to force a selection change. Requires `--sdr-profile`. |
| `--quit` | Ask the resident instance to exit. |
| `--help` | Show command-line help. |

For example, a display with two installed factory SDR profiles can use:

```text
toggleHDR.exe --tray --display "\\.\DISPLAY1" --sdr-profile "Factory-sRGB.icm" --sdr-reset-profile "Factory-P3.icm"
```

Replace the example filenames with profiles already associated with the selected monitor. Explicit SDR profile restoration is optional; no profile is chosen or installed automatically. HDR profile associations are separate and are not replaced by the SDR options. On multi-monitor systems, `--display` chooses the tray/menu target and profile-refresh target; it does not change which display Windows handles through its native shortcut.

To start the tray application at sign-in, place a shortcut to the executable with `--tray` and the desired options in the current user's Startup folder. Stop any previous shortcut helper before using the native handler.

## Build

Use Visual Studio 2022 with the **Desktop development with C++** workload and a Windows SDK, or a recent [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) toolchain. The executable uses standard Windows APIs, embeds its icons, and does not require administrator privileges.

### Visual Studio

Open `toggleHDR.sln` and build **Release | x64**. The project uses C++17 and the static C++ runtime.

### CMake

With Visual Studio available:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The CMake target also supports a Windows LLVM-MinGW toolchain. Linkage is static for the C++ runtime.

### Portable LLVM-MinGW

Extract the Windows x86-64 UCRT LLVM-MinGW distribution, then run:

```powershell
.\build-native.ps1 -ToolchainPath 'C:\Tools\llvm-mingw'
```

The executable is written to `build-native\toggleHDR.exe`. Use `-OutputDirectory` to choose another build directory. This script does not install the compiler or modify the system PATH.

The six committed tray icon files can be regenerated on Windows using `python tools\make-tray-icons.py`. Pillow and Windows' Bahnschrift font are needed only when regenerating icons, not when building or running the application. Each icon contains ten sizes for different display scales. Font files are not redistributed.

### Regression tests

From the repository root, compile and run the standalone display tests with the same portable LLVM-MinGW toolchain:

```powershell
$hdrTestCompiler = 'C:\Tools\llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe'
New-Item -ItemType Directory -Path build-native -Force | Out-Null
& $hdrTestCompiler -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 tests\DisplayTests.cpp -o build-native\DisplayTests.exe -luser32
if ($LASTEXITCODE -ne 0) { throw 'Test compilation failed.' }
& .\build-native\DisplayTests.exe
if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed.' }
```

`DisplayTests.cpp` includes `Display.cpp` directly, so do not compile it a second time in this command. The fixtures check HDR versus SDR/WCG detection, error handling, profile matching, and SDR-profile restoration after failures. Display-state queries and profile writes exercised by these tests are replaced with fakes; the tests do not change the real display.

## Implementation notes

On recent Windows versions, advanced color includes both HDR and SDR wide-color management. The application uses the newer HDR-specific query/set operations when supported, rather than assuming that the legacy advanced-color flag means HDR is active.

Display Settings can be hosted in `ApplicationFrameHost.exe`. Cleanup identifies its `SystemSettings.exe` child and closes the visible Settings window. If normal closure fails, it can stop that specific Settings process; it does not terminate the shared frame host.

The application reports Windows' display state and profile associations. Confirming that a calibration looks correct still requires checking the display itself.

## License

The upstream project and this derivative remain licensed under GNU GPL version 3 or later. See [LICENSE](LICENSE). Original copyright: Clayton Macleod, 2025.
