# Building MTGA Ultrawide Patcher 3.0.0

The application is C++17 with a native Win32 GUI. The Windows build links
its C++ runtime statically and uses Windows system DLLs. There is no .NET,
WebView, or GUI framework dependency. The supplied x64 executable is
559,616 bytes; another toolchain or different flags can change its size.

## Windows executable

Use an LLVM MinGW toolchain and put its `bin` directory on PATH. The release
was cross-compiled using LLVM MinGW 20260922, UCRT, x86_64 Windows target.
UCRT is included with Windows 10.

Run these commands **from the package's `source` directory**, so the resource
compiler finds `app.manifest`. Commands are shown for a shell; the two build
commands also work in a Windows command prompt with LLVM MinGW on PATH.

```sh
x86_64-w64-mingw32-windres resources.rc -O coff -o resources.o
x86_64-w64-mingw32-clang++ -std=c++17 -Os -DNDEBUG -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -static -mwindows -municode main.cpp gui.cpp engine.cpp matcher.cpp resources.o -o MTGAUltrawidePatcher.exe -Wl,--gc-sections -Wl,-s -luser32 -lgdi32 -lshell32 -lole32 -luuid -lcomctl32 -lbcrypt -ladvapi32
```

`resources.rc` embeds version information, common-controls styling, DPI
awareness, and an `asInvoker` manifest. The program does not automatically
elevate. To modify an installation under Program Files, launch it using
**Run as administrator**.

## Source map

| File | Purpose |
| --- | --- |
| `main.cpp` | GUI entry point and optional command-line actions |
| `gui.cpp` | Native controls, folder picker, DPI scaling, background worker |
| `matcher.cpp`, `matcher.h` | Bounds-checked PE/CLI metadata reader and method validation |
| `engine.cpp`, `engine.h` | Backup, hash verification, install, and restore |
| `resources.rc`, `app.manifest` | Version information and Windows manifest |

The matcher reads assembly bytes without loading or executing Core.dll.
It locates the target method through metadata and validates its expected
instruction pattern before returning a patch location. Unrelated layout
changes are tolerated; changes to the relevant logic are refused.

## Optional Linux development build

For backend inspection and testing, use a C++17 compiler and OpenSSL
development headers. The Windows GUI is excluded from this build:

```sh
g++ -std=c++17 -O2 -Wall -Wextra main.cpp engine.cpp matcher.cpp -lcrypto -o mtga-ultrawide
./mtga-ultrawide status --game-path /path/to/copied/MTGA
```

Linux tests exercise the portable matcher and filesystem backend. They do
not validate Win32 rendering, Windows process detection, permissions,
ReplaceFileW behavior, or actual game fullscreen behavior. The supplied
Windows executable still requires local runtime and gameplay testing.
