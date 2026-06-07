# webkbm

A tiny Windows system-tray app that lets your phone act as a wireless trackpad and keyboard for your PC. No app install on the phone — it serves a small PWA over HTTP, and a WebSocket carries mouse/keyboard events that are injected via `SendInput`.

Right-click the tray icon for a QR code; scan it with a phone on the same Wi-Fi.

## Build

Requires Windows with CMake and an MSVC toolchain (Visual Studio or the Build Tools).

```bat
build.bat            REM Debug
build.bat release    REM Release
```

Produces `webkbm.exe` in the repo root.

## Run

```bat
webkbm.exe
```

A tray icon appears (three blue dots over a bar). Right-click it:

- **Show QR code** — pops a window with the LAN URL as a QR; scan with the phone.
- **Exit** — quits.

The PWA exposes a trackpad area (one-finger move, two-finger scroll, tap/2-tap/3-tap for L/R/M click), L/Mid/Right buttons, a transient text input, and a row of special keys (Win/Esc/Bksp/Enter/Ctrl/Tab/Alt/Shift/arrows). Modifier keys are sticky: tap to arm, tap again to send standalone (e.g. tap-tap Win opens Start; tap Win then tap an arrow snaps a window).

Logs to `webkbm.log` next to the exe.
