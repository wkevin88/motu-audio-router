# MOTU Audio Router

A tiny Windows tray app that routes an input pair on a MOTU M4 to an output pair with low latency.

The M4's hardware direct monitoring only sends inputs to outputs 1-2. This app adds a software path, for example Line In 3-4 (game consoles) to Out 3-4, while Windows keeps playing normally on 1-2. It does what a DAW like REAPER could do, without running a DAW.

- About 4 ms round trip at the default 32-sample buffer (48 kHz).
- Uses about 14 MB of memory and almost no CPU.
- Runs in the system tray with no window, and can start with Windows.

## How it works

- The app talks to the MOTU M Series ASIO driver directly. It only opens the channels it routes, so Windows audio on Out 1-2 keeps working alongside it.
- On every audio callback it copies the selected input pair to the selected output pair (or writes silence when muted). It does nothing else on the audio thread: no allocation, locks or logging.
- With "Mix in PC audio" on, it also opens the M4's Loopback 1-2 inputs, which carry what Windows is playing. It adds them to the routed input, limiting the sum so loud peaks clip cleanly instead of distorting.
- Direct monitoring to Out 1-2 is untouched and stays zero-latency in hardware.
- A background thread owns the driver and keeps it running. It re-opens the driver after:
  - a driver reset request, which the M4 sends on unplug and some setting changes
  - a sample-rate change
  - a settings change from the menu
  - 2 seconds without audio callbacks, which covers sleep and resume
- If the device isn't available, for example at login or while unplugged, it retries every 2 seconds.

## Usage

Run `motu-audio-router.exe`. A speaker icon appears in the system tray. Hover over it for the status, or click it for the menu:

| Menu item | What it does |
|---|---|
| Status line | Current route, buffer size and round-trip latency, or "Waiting for MOTU M Series..." |
| Input | In 1-2 or In 3-4 |
| Output | Out 1-2 or Out 3-4 |
| Buffer size | 16, 32, 64, 128 or 256 samples (see below) |
| Mix in PC audio | Also plays the PC's audio (what Windows plays on Out 1-2) on the routed output |
| Mute | Silences the routed output instantly |
| Open driver settings | Opens the MOTU control panel (use it to change the sample rate) |
| Open log | Opens `log.txt` in your default text editor |
| Start with Windows | Launches the app when you sign in |
| Quit | Stops routing and exits |

Changing the input, output, buffer size or PC audio mixing re-opens the driver, which causes a gap of about 60 ms on the routed output only.

### Choosing a buffer size

Smaller buffers mean less delay but less slack for the computer. If the CPU is busy for longer than one buffer, you hear a click or pop. Round-trip latencies reported by the M4 driver at 48 kHz:

| Buffer (samples) | Round trip |
|---|---|
| 16 | 3.3 ms |
| 32 (default) | 4.0 ms |
| 64 | 6.0 ms |
| 128 | 10.0 ms |
| 256 | 15.3 ms |

About 2.6 ms of that is fixed USB and converter latency, so going below 32 gains little. If you hear crackles while gaming, step up to 64.

### Settings and log files

Both live in `%APPDATA%\motu-audio-router\`:

- `config.ini` holds the driver name, routing, buffer size, PC audio mixing and mute. It's saved on every menu change. Channel numbers are 0-based, so `inFirst=2` means In 3-4.
- `log.txt` records startup, each driver open with its reported latency, re-opens and errors. The previous session's log is kept as `log.old.txt`.

"Start with Windows" adds an entry for the app to your Windows sign-in startup list (the `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` registry key). Windows staggers those apps, so with many of them the router can take a minute or two to appear after you sign in.

## Building

Requirements:
- Windows 10 or 11
- Visual Studio with the C++ desktop workload (MSVC)
- CMake 3.20 or later
- The MOTU M Series driver

1. Download the [Steinberg ASIO SDK](https://www.steinberg.net/asiosdk) and extract it so that `third_party/asiosdk/common/iasiodrv.h` exists.
2. Build:
   ```
   cmake -S . -B build -A x64
   cmake --build build --config Release
   ```
3. The app is `build\Release\motu-audio-router.exe`.

The build also produces `spike.exe`, a console tool from the feasibility test. It routes audio for a set time and prints the driver's channels, latencies and any dropouts:

```
build\Release\spike.exe [seconds=20] [bufSize=driver default] [inFirst=2] [outFirst=2] [driver="MOTU M Series"]
```

## Project layout

| Path | Contents |
|---|---|
| [src/audio.cpp](src/audio.cpp) | ASIO engine: driver loading, the audio callback, the background thread, re-open and retry |
| [src/main.cpp](src/main.cpp) | Tray icon, menu and single-instance check |
| [src/config.cpp](src/config.cpp) | INI settings and the log file |
| [src/startup.cpp](src/startup.cpp) | "Start with Windows" |
| [spike/spike.cpp](spike/spike.cpp) | Standalone ASIO test tool |

## Limitations

- Supports one device only: the M4 is both input and output. There's no routing between different devices.
- Supports one route at a time.
- The only volume control is mute; there's no gain, including no separate volume for PC audio when mixing.
- PC audio mixing assumes the M4's channel layout, where Loopback 1-2 is ASIO inputs 5-6.
- PC audio can't be mixed into Out 1-2. Loopback 1-2 captures Out 1-2, so that would feed back into a loud squeal; the menu item is greyed out there and the app ignores the setting.
- The sample rate is set in the MOTU control panel, not in the app.
- Changing the buffer size can cause a short buzz on the routed output. It comes from the driver while it reconfigures; the app can't prevent it.

## Disclaimers

**AI usage.** Claude (Anthropic), through Claude Code, wrote most of this project's code and documentation. A person directed the work and tested the results on real hardware.

**Testing.** The app has only been tested on one setup:
- MOTU M4 with the MOTU M Series ASIO driver
- Windows 11
- 48 kHz, routing Line In 3-4 to Out 3-4

What was checked:
- routing alongside Windows audio
- buffer sizes 16-256
- unplugging and replugging the device
- starting at sign-in

Mixing in PC audio has only been checked to start correctly, not listened to. Sleep and resume, 96 kHz, long sessions at small buffer sizes and other audio interfaces have not been tested. The latency figures above are what the driver reports, not measured values.

The app is provided as is, with no warranty. It sends audio straight to your outputs, so start with your speaker or headphone volume low.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).

The ASIO SDK is licensed by Steinberg under either its proprietary ASIO License or GPLv3. It isn't part of this project's source; each build downloads it into `third_party/` as described above. ASIO is a trademark of Steinberg Media Technologies GmbH.
