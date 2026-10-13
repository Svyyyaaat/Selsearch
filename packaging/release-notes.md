First public build of Selsearch: freeze the screen, select text or part of an image, then copy, translate, search or ask a local AI about it. Plus an Alt+Space launcher.

## Downloads

| | File |
|---|---|
| Fedora 43, x86_64 | `selsearch-*.fc43.x86_64.rpm` |
| Windows 10/11, x64 (installer) | `Selsearch-*-windows-x64-setup.exe` |
| Windows 10/11, x64 (portable) | `Selsearch-windows-x64.zip` |
| Arch | build `packaging/arch/PKGBUILD` with `makepkg -si` |
| Other Linux | build from source, see [INSTALL.md](https://github.com/svyyyaaat/selsearch/blob/@TAG@/INSTALL.md) |

**Fedora:**

```sh
sudo dnf install ./selsearch-*.fc43.x86_64.rpm
systemctl --user enable --now selsearch.service
selsearch config
```

**Windows:** run the installer, or unzip anywhere and start `selsearchd.exe`.
Keys: Alt+Space launcher, Alt+Shift+S search the screen, Alt+Shift+T select text.

On first start a welcome page lets you download the text recognition models
(12–25 MB) and set the shortcuts.

## Known limits

- The Windows build has been tested under Wine, not yet on real Windows.
  Please report hotkey, focus or multi-monitor DPI problems.
- GNOME has no layer-shell, so the overlay can't cover everything there; the
  launcher and capture work.
