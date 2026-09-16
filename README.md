# M5Dial MPD client
<img width="384" height="512" alt="mpdremote1" src="https://github.com/user-attachments/assets/59f3c87b-a7ed-4ebd-8933-2f8c7efccc82" />

<img width="384" height="512" alt="mpdremote2" src="https://github.com/user-attachments/assets/91647093-f9b1-40d6-9a6f-60abe9fe14a6" />


<img width="384" height="512" alt="mpdremote3" src="https://github.com/user-attachments/assets/47800bf3-b5d4-435e-8b34-dcd98cf3031a" />



A compact [MPD](https://www.musicpd.org/) remote-control client for the
**M5Stack M5Dial** (ESP32-S3, 1.28" round GC9A01 display, rotary encoder +
push button).

Displays the current track and a progress ring, controls playback and volume,
lets you browse the music library, filesystem and stored playlists (M.A.L.P.
style), and reconnects automatically.

## Features

- **Now-playing screen** — artist / title / album, smooth progress ring,
  elapsed + total time, play state, volume, connection status LEDs.
  Long titles scroll (marquee) around the round screen.
- **Playback control** — play/pause, next, previous, stop, volume.
- **Main menu** — hold in the now-playing screen for a menu hub:
  Queue, Files, Library, Playlists, Back.
- **Queue view** — scroll the MPD queue, press to play an entry.
- **Files browser** — walk the music directories (`lsinfo`), drill into
  folders, enqueue or play songs / folders / `.m3u` playlists.
- **Library browser** — artists → albums → songs via the MPD tag database
  (`list` / `find`, with albumartist + artist fallback).
- **Playlist browser** — list stored playlists, preview their songs, enqueue
  or play songs, or double-press a playlist to *load & play* it.
- **Click semantics** — one click = gentle action (enqueue / open), two quick
  clicks = stronger action (play now / load & play). Hold = back.
- **Auto-reconnect** — Wi-Fi and MPD both retry with back-off; the client
  uses MPD's `idle` command (incl. `database` / `stored_playlist`) so the UI
  updates the moment something changes.
- **PSRAM storage** — the queue and browse listings are kept in ESP-PSRAM as
  raw protocol text, so very large libraries don't eat the internal heap.

## Hardware / wiring

None — it's an M5Dial. Just power it via USB-C.

## Controls

| Input              | Now playing                    | Menu / Queue / Browser        |
|--------------------|--------------------------------|-------------------------------|
| Knob rotate        | Volume                         | Scroll selection              |
| Knob push (short)  | Play / pause                   | Open / enqueue / play         |
| Knob push (2×)     | –                              | Play now / load & play (files, songs, playlists) |
| Knob push (long)   | Open main menu                 | Back (up one level)           |

Navigating the browser: rotate to move, click to open a folder / artist /
album / playlist preview, single-click a song or file to add it to the queue,
double-click it to play it right away. Hold anywhere in a browser to go back
one level (and eventually back to the menu; hold in the menu goes back to the
now-playing screen).

## Build & flash

```bash
pio run                 # install toolchain + libs, compile
pio run -t upload       # flash over USB (native USB-CDC)
pio device monitor      # serial log at 115200 baud
```

Requires [PlatformIO Core](https://platformio.org/install) ≥ 6.x.
The board definition lives in `boards/m5stack-dial.json` (mirrors the
official Arduino `M5Stack Dial` board: ESP32-S3-FN8, 8 MB flash, OPI PSRAM).
The pinned arduino-esp32 fork does not ship the `m5stack_dial` pin map, so it
is provided project-locally in `variants/m5stack_dial/pins_arduino.h` and wired
in via the board's `build.variants_dir`.
Version-pin `platform = espressif32@7.1.3` if you need a reproducible build.

## Configuration

Edit `include/config.h` before flashing:

```c
#define WIFI_SSID      "myssid"        // your 2.4 GHz SSID
#define WIFI_PASS      "mypassword"
#define MPD_HOST       "192.168.1.10"  // IP or hostname of the MPD machine
#define MPD_PORT       6600
#define MPD_PASSWORD   ""              // optional MPD password
```

MPD must be listening on TCP 6600 on your network (`bind_to_address` in
`mpd.conf`, e.g. `"any"` for LAN access). No extra MPD plugins required.

## Notes

- The default fonts are the DejaVu/Latin sets bundled with M5GFX; German
  umlauts render fine (DejaVu covers them). Non-Latin scripts would need the
  bundled `efont` fonts.
- The buzzer and IMU are not used in v1.
- If PSRAM ever fails to initialise, the UI falls back to an internal-RAM
  framebuffer and only loses the big-playlist capability.

## Project layout

```
boards/        custom M5Dial board definition (PlatformIO)
include/       config.h
variants/      project-local m5stack_dial pin map (pins_arduino.h)
lib/MpdClient/ MPD protocol client + PSRAM playlist parser
src/           main, background MPD task, round-screen UI
```

example include/config.h:


```
#pragma once

// =====================================================================
//  M5Dial MPD client - configuration
//
//  Adjust the values below to match your network and MPD setup, then
//  build with `pio run` and flash with `pio run -t upload`.
// =====================================================================

// ---- Wi-Fi -----------------------------------------------------------
#define WIFI_SSID "YOUR_WIFI_SSD"
#define WIFI_PASS "YOUR_WIFI_PASSWD"

// ---- MPD server ------------------------------------------------------
// IP address or hostname of the machine running MPD (port 6600).
#define MPD_HOST "192.168.23.1"
#define MPD_PORT 6601
// Optional MPD password (leave empty if MPD has no password set).
#define MPD_PASSWORD ""

// ---- MPD instances ---------------------------------------------------
// One MPD per room, each on its own port.  Define the port for each room,
// then add both the on-screen name and the define to MPD_INSTANCES below.
#define Room1           6600
#define Room2        6601
#define Room3      6603
#define Room4              6602
#define Room5          6607
#define Room6            6605
#define Room7          6604
#define Room8            6606

// List shown in the menu: { display name, port define }.
// The first entry is the default instance used at boot.
#define MPD_INSTANCES                        \
    { "Office", Room1 },                   \
    { "LivingRoom", Room2 },            \
    { "SleepingRoom", Room3 },        \
    { "Bath", Room4 },                         \
    { "Bar", Room5 },                  \
    { "Kitchen", Room6 },                     \
    { "Basement", Room7 },                  \
    { "AllRooms", Room8 }
```



