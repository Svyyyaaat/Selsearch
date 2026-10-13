# Selsearch

Select anything on your screen — text, an image, a whole window — and copy it,
translate it in place, search it, or ask an AI about it. Plus an Alt+Space
launcher for apps, files, maths and the web.

A de-Googled take on the "Google app for desktop" / Circle to Search
experience, built for Linux and Wayland (Hyprland first). Recognition and
translation run on your computer. Nothing leaves it unless you search the web
or point it at a remote AI.

## Features

**Lens** (`selsearch lens`)
- Freezes every monitor instantly (direct compositor capture, correct with
  fractional scaling and rotated outputs).
- Drag a rectangle or **click a window**.
- Text on screen is recognised in the background the moment the lens opens:
  **drag across words to select them** like normal text, double-click selects a
  line, Ctrl+A selects all.
- Actions: search (text, or the image with your chosen engine), copy text,
  copy image, **translate in place** (painted over the original), ask AI, save.
- Keys: `Enter` search · `Ctrl+C` copy · `Ctrl+Shift+C` copy text · `T`
  translate · `A` ask AI · `Ctrl+S` save · `Esc` back/close.

**Launcher** (`selsearch launcher`)
- Apps (`.desktop`, frecency-ranked, desktop actions), files (plocate or a local
  index), calculator and unit conversion (`5 km in mi`, `20% of 150`,
  `0x1f in dec`), URLs, web search.
- `Tab` switches to AI chat; `Ctrl+Enter` asks the AI directly. Local chat
  history.
- Results open in a built-in panel (QtWebEngine, off-the-record by default) or
  your browser.

**Choose your providers**
- Web: DuckDuckGo (default), Google, Brave, Kagi, Bing, Startpage, Yandex,
  Ecosia, Qwant, Mojeek, your SearXNG, or any URL template.
- Image search: Google Lens, Yandex, Bing, TinEye — only on your action (or
  automatically, if you enable "After selecting → Search the image").
- OCR: PP-OCRv5 (ONNX Runtime, best for screenshots; Latin, Cyrillic, CJK,
  Arabic, Devanagari, Greek, Thai…) with Tesseract as fallback.
- Translation: Firefox Translations models via translateLocally, a
  LibreTranslate server, or your AI model.
- AI: any OpenAI-compatible endpoint — ollama, llama.cpp, LM Studio, vLLM… A
  footer always says whether answers are local or which host gets your data.

## Privacy

| | Google app for desktop | Selsearch |
|---|---|---|
| Text recognition | Google servers | on device |
| Keystrokes | sent to Google for suggestions | stay local (suggestions are opt-in) |
| Browser data | reads Chrome bookmarks & account | never |
| Clipboard / selected text | watched | only when you copy |
| Telemetry, persistent ID | yes | none |
| Autostart | on by default | opt-in |

## Install

Arch: `packaging/arch/PKGBUILD` (AUR-ready, `selsearch-git`).

From source (Qt ≥ 6.8, layer-shell-qt ≥ 6.0, wayland, onnxruntime and/or
tesseract; qt6-webengine optional):

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build && sudo cmake --install build
systemctl --user enable --now selsearch.service   # or start selsearchd from your compositor
```

Then bind the keys. Hyprland 0.55+ (`~/.config/hypr/hyprland.lua`):

```lua
hl.bind("ALT + Space", hl.dsp.exec_cmd("selsearch launcher"))
hl.bind("SUPER + S", hl.dsp.exec_cmd("selsearch lens"))
hl.bind("SUPER + SHIFT + S", hl.dsp.exec_cmd("selsearch lens --text"))
-- optional: make the lens appear instantly (no layer animation)
-- hl.layer_rule({ match = { namespace = "selsearch-lens" }, name = "selsearch-lens", no_anim = true })
```

sway/river/niri: `bindsym Mod1+space exec selsearch launcher` etc.
KDE/GNOME: add custom shortcuts running the same commands (capture goes through
the xdg-desktop-portal Screenshot API there; GNOME has no layer-shell, so the
overlay is best on wlroots compositors and KDE).

**Windows 10/11**: run the installer or unzip `Selsearch-windows-x64.zip` (CI
artifacts) and start `selsearchd.exe`. It registers its own hotkeys (Alt+Space,
Alt+Shift+S, Alt+Shift+T; the `shortcuts.*` config keys), sits in the tray and
can start with Windows. Building and the differences from Linux:
[docs/BUILDING-windows.md](docs/BUILDING-windows.md).

On first start Selsearch opens a short welcome page; `selsearch config` opens
the settings. Every option is also a key in `~/.config/selsearch/config.toml`.

## How it works

- `selsearchd` — resident daemon (C++20, Qt 6/QML). Keeps the overlay windows
  and (lazily) the models warm, so a key press shows the UI within a frame.
  Models unload after a minute idle.
- `selsearch` — tiny client without Qt, sends one command over
  `$XDG_RUNTIME_DIR/selsearch.sock` and starts the daemon on demand.
- `selsearch-web` — results panel, started only while results are on screen.
- Capture: `ext-image-copy-capture-v1`, falling back to `wlr-screencopy`
  (Hyprland, sway, river, niri, …), then the portal (GNOME, KDE).
- Window list for snapping: Hyprland and sway IPC.

## Development

```sh
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

`tests/tools/` has helpers for headless UI testing: `selsearch-capture-dump`
(grab outputs to PNG) and `selsearch-vptr` (drive a virtual pointer in a
wlroots compositor, e.g. `sway` with `WLR_BACKENDS=headless`).

## Licence

MIT. Icons: [Lucide](https://lucide.dev) (ISC). Config parsing:
[toml++](https://github.com/marzer/tomlplusplus) (MIT). Recognition models:
PaddleOCR PP-OCRv5 (Apache-2.0), downloaded on request.
