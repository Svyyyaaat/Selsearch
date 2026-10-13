# Installing Selsearch

- [Linux](#linux): [Fedora](#fedora) · [Arch](#arch-linux) · [Debian / Ubuntu](#debian--ubuntu) · [Other distros](#other-distros)
- [Windows](#windows)
- [First run](#first-run) · [Shortcuts](#shortcuts) · [Updating](#updating) · [Uninstalling](#uninstalling) · [Troubleshooting](#troubleshooting)

## Requirements

| | Needed | Optional |
|---|---|---|
| Qt | ≥ 6.8: base, declarative, svg, wayland, shadertools | webengine (results panel inside Selsearch) |
| Wayland | layer-shell-qt ≥ 6.0, wayland, wayland-protocols | |
| Text recognition | onnxruntime (PP-OCRv5) **or** tesseract + leptonica | both; ICU (Chinese/Japanese word splitting) |
| Build | CMake ≥ 3.25, Ninja, a C++20 compiler | |

Best on Hyprland, sway, river, niri and KDE Plasma (Wayland). GNOME works
for the launcher and capture, but GNOME has no layer-shell, so the overlay
can't sit above everything there.

## Linux

### Fedora

**Ready-made package (Fedora 43):** download `selsearch-*.fc43.x86_64.rpm`
from the [latest release](https://github.com/svyyyaaat/selsearch/releases), then

```sh
sudo dnf install ./selsearch-*.fc43.x86_64.rpm
systemctl --user enable --now selsearch.service
selsearch config
```

**From source** (Fedora 41 or newer, Qt 6.8+):

```sh
sudo dnf install git gcc-c++ cmake ninja-build pkgconf-pkg-config \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel qt6-qtsvg-devel \
  qt6-qtwayland-devel qt6-qtshadertools-devel qt6-qtwebengine-devel \
  layer-shell-qt-devel wayland-devel wayland-protocols-devel libicu-devel \
  onnxruntime-devel tesseract-devel leptonica-devel tesseract-langpack-eng
```

If `onnxruntime-devel` isn't available on your release, see
[ONNX Runtime without a package](#onnx-runtime-without-a-package). Add
`tesseract-langpack-<lang>` (e.g. `-rus`, `-jpn`) for the languages you read.

Then [build and install](#build-and-install).

### Arch Linux

```sh
sudo pacman -S --needed git cmake ninja qt6-base qt6-declarative qt6-svg qt6-wayland \
  qt6-shadertools qt6-webengine layer-shell-qt wayland wayland-protocols icu \
  onnxruntime tesseract tesseract-data-eng leptonica
```

Or build the package: `cd packaging/arch && makepkg -si`.

### Debian / Ubuntu

Needs Qt 6.8, i.e. Debian 13 (trixie) or Ubuntu 25.04 or newer. Ubuntu 24.04
ships Qt 6.4, which is too old.

```sh
sudo apt install git g++ cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-private-dev qt6-declarative-dev qt6-svg-dev qt6-wayland-dev \
  qt6-shadertools-dev qt6-webengine-dev liblayershellqtinterface-dev \
  libwayland-dev wayland-protocols libicu-dev \
  libtesseract-dev libleptonica-dev tesseract-ocr-eng
```

ONNX Runtime isn't packaged there: see
[ONNX Runtime without a package](#onnx-runtime-without-a-package).

### Other distros

Install the [requirements](#requirements) under your distro's names, then build.

### Build and install

```sh
git clone https://github.com/svyyyaaat/selsearch.git
cd selsearch
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DSELSEARCH_BUILD_TESTS=OFF
cmake --build build
sudo cmake --install build
systemctl --user enable --now selsearch.service
selsearch config
```

`systemctl --user enable --now` starts the background service now and at
every login. If you'd rather start it from your compositor, skip that line
(the Hyprland snippet in Settings includes an autostart line).

To try it without installing: `./build/selsearchd &` then `./build/selsearch lens`.

### ONNX Runtime without a package

PP-OCRv5 (the better text recognition) needs ONNX Runtime. Without it,
Selsearch builds with Tesseract only. To use the official build:

```sh
curl -LO https://github.com/microsoft/onnxruntime/releases/download/v1.23.2/onnxruntime-linux-x64-1.23.2.tgz
tar xzf onnxruntime-linux-x64-1.23.2.tgz -C ~
# then add this to the cmake -B build line:
#   -DCMAKE_PREFIX_PATH=$HOME/onnxruntime-linux-x64-1.23.2
```

If CMake complains about `lib64` or `include/onnxruntime` paths in that
package, run `ln -s lib ~/onnxruntime-linux-x64-1.23.2/lib64` and
`ln -s . ~/onnxruntime-linux-x64-1.23.2/include/onnxruntime`.

## Windows

Windows 10 (1809+) or 11, x64.

1. From the [latest release](https://github.com/svyyyaaat/selsearch/releases)
   download either
   - `Selsearch-<version>-windows-x64-setup.exe` (installer: Start Menu entry, uninstaller), or
   - `Selsearch-windows-x64.zip` (portable: unpack anywhere).

   Builds of every commit are also under the repository's **Actions** tab →
   a **build** run → *Artifacts*.
2. Run `selsearchd.exe` (the installer offers to start it). A tray icon appears;
   right-click it for the launcher, screen search, Settings and Quit.
3. In Settings → Appearance, turn on **Start with Windows**.

Default keys on Windows: **Alt+Space** launcher, **Alt+Shift+S** search the
screen, **Alt+Shift+T** select text. (Win+S and Win+Shift+S belong to
Windows Search and the Snipping Tool, and Windows won't hand them to an app.)
While Selsearch runs, Alt+Space no longer opens a window's system menu, the
same as with PowerToys Run; pick another key in Settings if you need it.

Settings, models and history live in `%LOCALAPPDATA%\selsearch\`.
Building it yourself (MSVC, or cross-compiling from Linux):
[docs/BUILDING-windows.md](docs/BUILDING-windows.md).

## First run

On first start a welcome page opens (later: `selsearch config`).

1. **Shortcuts.** See [below](#shortcuts).
2. **Text recognition models.** Click *Download* (about 12–25 MB, depending on
   your languages). Pick your languages under *Text recognition* first;
   most-used first.
3. **AI (optional).** Point it at any OpenAI-compatible server, e.g.
   [ollama](https://ollama.com): `ollama pull qwen3-vl:4b`, then *Test*.
   Turn AI off entirely under *AI* if you don't want it.
4. **Translation.** Works out of the box by opening Google Translate or DeepL
   with your selection. For fully local, in-place translation install
   [translateLocally](https://translatelocally.com) or run a
   [LibreTranslate](https://libretranslate.com) server.

Everything is also in `~/.config/selsearch/config.toml`
(`%LOCALAPPDATA%\selsearch\config.toml` on Windows).

## Shortcuts

Change them in **Settings → Shortcuts**: click one and press the new keys.

| Desktop | How they take effect |
|---|---|
| Windows | Immediately: Selsearch registers them itself. If another program already holds a key, Settings keeps it and the log says `not registered`. |
| KDE Plasma, GNOME 48+ | The desktop asks once to confirm; you can also change them in its keyboard settings. |
| Hyprland, sway | The compositor owns global keys: copy the generated snippet from Settings into its config. |
| Others (niri, river, …) | Bind `selsearch launcher`, `selsearch lens`, `selsearch lens --text`. |

Hyprland 0.55+ (`~/.config/hypr/hyprland.lua`), default keys:

```lua
hl.bind("ALT + Space", hl.dsp.exec_cmd("selsearch launcher"))
hl.bind("SUPER + S", hl.dsp.exec_cmd("selsearch lens"))
hl.bind("SUPER + SHIFT + S", hl.dsp.exec_cmd("selsearch lens --text"))
```

Selsearch's surfaces use the layer namespaces `selsearch-lens`,
`selsearch-launcher` and `selsearch-results`, so your layer animations and
layer rules apply to them.

## Updating

```sh
cd selsearch
git pull
cmake -B build
cmake --build build
sudo cmake --install build
systemctl --user restart selsearch.service
```

## Uninstalling

```sh
systemctl --user disable --now selsearch.service
cd selsearch && sudo xargs rm -v < build/install_manifest.txt
rm -r ~/.config/selsearch ~/.local/share/selsearch ~/.local/state/selsearch   # settings, models, history
```

## Troubleshooting

- **`selsearch: selsearchd is not running`.** Start it: `systemctl --user start
  selsearch.service`, or run `selsearchd` in a terminal to see errors.
- **The overlay opens as a normal window / behind other windows.** The
  layer-shell plugin wasn't found: make sure `layer-shell-qt` is installed for
  the same Qt as Selsearch.
- **Text isn't selectable.** Check *Settings → Text recognition*. The status
  line says which engine and model are used and what's missing.
- **Logs.** `journalctl --user -u selsearch -f`, or run `QT_LOGGING_RULES="selsearch.*=true" selsearchd`.
