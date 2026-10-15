# Selsearch on Windows

Selsearch runs on Windows 10 (1809+) and 11 with the same UI as on Linux: an
Alt+Space launcher and a lens that freezes the screen. This page covers how the
port differs, how to build it with MSVC, and how to cross-compile it from Linux
to check that it still builds.

## How it differs from Linux

| | Linux (Wayland) | Windows |
|---|---|---|
| Hotkeys | the compositor's config runs `selsearch …`, or the GlobalShortcuts portal | `selsearchd` registers them itself (`RegisterHotKey`), from the `shortcuts.*` config keys |
| Screen capture | ext-image-copy-capture / wlr-screencopy, then the portal | GDI `BitBlt` per monitor, at physical resolution |
| Overlay windows | layer-shell surfaces | frameless, always-on-top tool windows (`src/app/WindowPlacement.cpp`) |
| Snap to windows | Hyprland / sway IPC | `EnumWindows` + DWM frame bounds, cloaked windows skipped |
| Apps in the launcher | `.desktop` files | Start Menu shortcuts (`.lnk`, `.url`) from `%APPDATA%` and `%ProgramData%`, shell icons |
| File search | plocate or the in-process index | the in-process index (`FindFirstFileEx`; hidden/system entries and junctions skipped) |
| IPC | `$XDG_RUNTIME_DIR/selsearch.sock` | named pipe `\\.\pipe\selsearch-<username>` |
| Starting with the session | `selsearch.service` (systemd user unit) | Settings → Appearance → "Start with Windows" (`HKCU\…\Run`), plus a tray icon |
| Config, models, history | `~/.config`, `~/.local/share`, `~/.local/state` | `%LOCALAPPDATA%\selsearch\` |
| CJK word breaks | ICU (`icu-uc`) | Windows' own ICU (`icu.h`, MSVC builds) or the `QTextBoundaryFinder` fallback |
| Tesseract | on by default | off by default (`-DSELSEARCH_WITH_TESSERACT=ON` with a pkg-config Tesseract) |

Default hotkeys on Windows: **Alt+Space** launcher, **Alt+Shift+S** search the
screen, **Alt+Shift+T** select text. Win+S and Win+Shift+S (the Linux defaults)
belong to Windows Search and the Snipping Tool, which Windows does not let an
app take. If a key is already taken by another program, `selsearchd` logs
`shortcut … not registered` and the other keys still work.

Alt+Space normally opens the active window's system menu (Move, Size, Close…).
While Selsearch holds the hotkey it wins, like PowerToys Run does; quit
Selsearch or change `shortcuts.launcher` to get the menu back.

Releasing Alt or Win after a hotkey would normally open the focused window's
menu bar or the Start menu, because the hotkey's own key never reaches
anyone. On every hotkey with Alt or Win, `WindowsShortcuts.cpp` injects an
unassigned key (0xE8, as AutoHotkey does) while the modifier is still down.

The key recorder in Settings → Shortcuts reads keys through a low-level
keyboard hook while it records, so Alt+…, Win+… and keys another program
already holds are recorded instead of acted on. The hook swallows every key
until a key is recorded, Esc is pressed, the window loses focus, or 20 s pass.
A key Windows or another program won't give up is shown under its row.

Focus: Windows only lets the program that received the last input bring a
window to the front. A hotkey or a tray click counts; for commands sent with
`selsearch.exe` the CLI calls `AllowSetForegroundWindow` first. As a fallback
`WindowPlacement.cpp` briefly attaches to the foreground thread's input
(`AttachThreadInput`).

DPI: Qt 6 makes the process Per-Monitor-V2 DPI aware, so GDI and the window
list work in physical pixels; Qt keeps every screen's top-left corner in
physical pixels and scales only its size, which is how monitors are matched to
`QScreen`s and window rectangles converted to Qt's logical coordinates, also
with mixed scale factors.

## Building with MSVC

Prerequisites:

- Visual Studio 2022 (Desktop development with C++), CMake ≥ 3.25, Ninja
  (both ship with Visual Studio).
- Qt 6.8.3 for `msvc2022_64` with the modules Qt Shader Tools, Qt WebEngine,
  Qt WebChannel and Qt Positioning (the last three only for the results panel).
  With aqtinstall:
  `aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtshadertools qtwebengine qtwebchannel qtpositioning -O C:\Qt`
- ONNX Runtime 1.23.2: unpack `onnxruntime-win-x64-1.23.2.zip` from the
  [releases](https://github.com/microsoft/onnxruntime/releases).

In an "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_PREFIX_PATH="C:\Qt\6.8.3\msvc2022_64;C:\path\to\onnxruntime-win-x64-1.23.2"
cmake --build build
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%
ctest --test-dir build --output-on-failure
```

All programs land in `build\bin`, together with a copy of `onnxruntime.dll`:
Windows looks for DLLs next to the executable first, and `System32` ships an
older `onnxruntime.dll` that would otherwise be picked up.

Run `build\bin\selsearchd.exe`; a tray icon appears. `selsearch.exe` is the
command-line client (`selsearch status`, `selsearch lens`, …).

### Packaging

```bat
mkdir staging
copy build\bin\selsearchd.exe staging & copy build\bin\selsearch.exe staging
copy build\bin\selsearch-web.exe staging & copy build\bin\onnxruntime.dll staging
windeployqt --release --qmldir qml --compiler-runtime --dir staging staging\selsearchd.exe
windeployqt --release --qmldir qml\web --compiler-runtime --dir staging staging\selsearch-web.exe
iscc /DAppVersion=0.1.0 /DSourceDir=%CD%\staging packaging\windows\selsearch.iss
```

The installer (Inno Setup 6) installs per user into
`%LOCALAPPDATA%\Programs\Selsearch` without admin rights. CI does all of this,
see `.github/workflows/build.yml`; it uploads `Selsearch-windows-x64.zip` and
the setup program as artifacts.

## Cross-compiling from Linux (MinGW-w64)

Useful to check that the Windows code compiles and links without a Windows
machine. It builds `selsearchd.exe`, `selsearch.exe`, the tests and the dev
tools; not `selsearch-web.exe`, because Qt WebEngine does not exist for MinGW.

```sh
# Toolchain (posix threads: std::thread / std::mutex need it)
sudo apt-get install -y g++-mingw-w64-x86-64-posix mingw-w64-tools
# Qt for the target, plus Qt for the host (moc, qmlcachegen, qsb...)
aqt install-qt windows desktop 6.8.3 win64_mingw -m qtshadertools -O /opt/qtwin
aqt install-qt linux desktop 6.8.3 linux_gcc_64 -m qtshadertools -O /opt/qt
# ONNX Runtime: the MSVC build links fine (C API, header-only C++ wrapper)
curl -LO https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-win-x64-1.23.2.zip
mkdir -p /opt/onnxruntime-win && unzip -d /opt/onnxruntime-win onnxruntime-win-x64-1.23.2.zip

cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DQT_HOST_PATH=/opt/qt/6.8.3/gcc_64 \
      -DCMAKE_PREFIX_PATH="/opt/qtwin/6.8.3/mingw_64;/opt/onnxruntime-win/onnxruntime-win-x64-1.23.2"
cmake --build build-win
```

### Running it under Wine

Good enough for a smoke test (the IPC pipe, the tests, capture and rendering);
not a substitute for real Windows (hotkeys, focus, DPI, the tray).

```sh
sudo apt-get install -y wine64 xvfb
export WINEDEBUG=-all LANG=C.UTF-8 LC_ALL=C.UTF-8
# The toolchain's runtime DLLs first, then Qt's.
export WINEPATH='Z:\usr\lib\gcc\x86_64-w64-mingw32\13-posix;Z:\usr\x86_64-w64-mingw32\lib;Z:\opt\qtwin\6.8.3\mingw_64\bin'
export QT_PLUGIN_PATH='Z:\opt\qtwin\6.8.3\mingw_64\plugins'
export QML_IMPORT_PATH='Z:\opt\qtwin\6.8.3\mingw_64\qml'
cd build-win/bin
/usr/lib/wine/wine64 selsearchd.exe --version
QT_QPA_PLATFORM=offscreen /usr/lib/wine/wine64 tst_core.exe
# GDI needs a display: run the daemon under Xvfb with the native platform
xvfb-run -a sh -c '/usr/lib/wine/wine64 selsearchd.exe & sleep 15;
    /usr/lib/wine/wine64 selsearch.exe status; /usr/lib/wine/wine64 selsearch.exe lens;
    sleep 3; /usr/lib/wine/wine64 selsearch-capture-dump.exe shots; /usr/lib/wine/wine64 selsearch.exe quit'
```

MinGW notes:

- Use the `-posix` compilers; the `-win32` thread model has no `std::thread`.
- `onnxruntime.lib` (an MSVC import library) links with GNU ld; the toolchain
  file adds `.lib` to the library suffixes so `find_library` sees it.
- MinGW has no `icu.h`, so CJK word breaks use the `QTextBoundaryFinder`
  fallback there (MSVC builds use Windows' ICU when the SDK provides it).
- Wine needs a UTF-8 locale for non-ASCII file names (`tst_launcher`'s
  `Résumé.odt`), and an X display (Xvfb) for GDI capture.
- `tst_paddleocr` compares word boxes with fonts rendered at test time; under
  Wine's fonts one box (`?`, ru-dark-13px) misses the IoU threshold.
