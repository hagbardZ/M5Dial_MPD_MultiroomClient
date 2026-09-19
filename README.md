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
  elapsed + total time, play state, volume.  Long titles scroll (marquee)
  around the round screen.
- **Webradio** — streams show the station name instead of the artist, and
  the real wall-clock time (NTP-synced, timezone offset configurable)
  instead of the meaningless stream play time.
- **Playback control** — play/pause, next, previous, stop, volume, and a
  repeat toggle that always repeats the **current song** (`repeat` + `single`),
  never the whole queue.
- **Main menu** — hold in the now-playing screen for a menu hub:
  Queue, Files, Library, Playlists, Back.
- **Idle auto-return** — after `MENU_TIMEOUT_MS` without any input, every
  screen other than now playing falls back to it automatically.  The
  file/library/playlist browsers use the longer `BROWSE_TIMEOUT_MS` so you
  have time to look around.
- **Standby** — hold the knob for `STANDBY_PRESS_MS` and the dial sleeps
  (screen + Wi-Fi off); wake by pressing the knob or turning the wheel.
  Two low-power engines are selectable in `config.h`: *light sleep* for an
  instant wake (~1 mA) or *deep sleep with an RTC-timer poll* (the chip
  idles at ~7 µA, wakes itself every `STANDBY_POLL_MS` to glance at the
  knob/wheel, and dozes again).  The knob/wheel sit on digital-only pins,
  so deep sleep can never be *triggered* directly from them — the poll is
  the trade-off that makes µA-class chip power possible without a wire.
- **Queue view** — scroll the MPD queue, press to play an entry.
- **Files browser** — walk the music directories (`lsinfo`), drill into
  folders, enqueue or play songs / folders / `.m3u` playlists.  Tapping the
  row counter at the bottom rescans the current folder in the MPD database
  (whole database while at the root), like the equivalent action in M.A.L.P.
- **Library browser** — artists → albums → songs via the MPD tag database
  (`list` / `find`, with albumartist + artist fallback).
- **Playlist browser** — list stored playlists, preview their songs, enqueue
  or play songs, or press a playlist to clear the queue and *load & play* it.
- **Click semantics** — one click = gentle action (enqueue / open / in a
  playlist: clear the queue + load & play), two quick clicks = play now for
  files, songs and folders. Hold = back.
- **Auto-reconnect** — Wi-Fi and MPD both retry with back-off; the client
  uses MPD's `idle` command (incl. `database` / `stored_playlist`) so the UI
  updates the moment something changes.
- **Memory-conscious** — runs in the ~128 KB internal-RAM heap of the
  PSRAM-less ESP32-S3-FN8.  Directory listings are buffered compactly
  (the `lsinfo` response is filtered down to `directory:` / `file:` /
  `playlist:` lines, dropping per-file tag blocks), so even large folders
  fit without OOM.

## Hardware / wiring

None — it's an M5Dial. Just power it via USB-C.

## Controls

| Input              | Now playing                    | Menu / Queue / Browser        |
|--------------------|--------------------------------|-------------------------------|
| Knob rotate        | Volume                         | Scroll selection              |
| Knob push (short)  | Play / pause                   | Open / enqueue / play         |
| Knob push (2×)     | –                              | Play now / load & play (files, songs, playlists) |
| Knob push (long)   | Open main menu                 | Back (up one level)           |
| Knob push (very long, ≥ `STANDBY_PRESS_MS`) | Standby (sleep)     | Standby (sleep)               |
| Touch              | Prev/next arrows; tap "playing" for play menu; tap the title for the queue; tap room name to switch rooms | Tap an entry to select / play; tap the bottom counter in the file browser to rescan the folder |

Navigating the browser: rotate to move, click to open a folder / artist /
album, single-click a playlist to clear the queue and play it, single-click a
song or file to add it to the queue, double-click it to play it right away.
Hold anywhere in a browser to go back
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
official Arduino `M5Stack Dial` board).  Note: the M5Dial's ESP32-S3 module
is the **FN8** variant — 8 MB flash and **no PSRAM** — so the firmware is
tuned for the ~128 KB internal-RAM heap and never relies on PSRAM.
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
#define MPD_PORT       6600            // default instance port
#define MPD_PASSWORD   ""              // optional MPD password

#define NTP_SERVER         "pool.ntp.org"  // NTP for the webradio wall clock
#define TIMEZONE_UTC_HOURS 2              // GMT offset (Germany: CEST=2, CET=1)
#define MENU_TIMEOUT_MS    5000           // idle auto-return (menu/queue/play menu); 0 = off
#define BROWSE_TIMEOUT_MS  15000          // idle auto-return while browsing files/library/playlists; 0 = off

// --- Standby ---
#define STANDBY_PRESS_MS  2000   // very-long knob hold → standby; 0 = off
#define STANDBY_DEEP_POLL 1      // 1 = deep sleep + RTC-timer poll, 0 = light sleep (instant wake)
#define STANDBY_POLL_MS   1000   // knob/wheel scan interval in deep-poll mode
```

MPD must be listening on TCP 6600 on your network (`bind_to_address` in
`mpd.conf`, e.g. `"any"` for LAN access). No extra MPD plugins required.

Rooms are defined in `MPD_INSTANCES` (one MPD process per port). The first
entry is used at boot; switch rooms by tapping the room name shown at the top
of the now-playing screen (`MODE_INSTS`).

## Notes

- The bundled M5GFX DejaVu fonts are ASCII-only (no umlauts), so the project
  ships umlaut-capable variants in `src/fonts/` — `DejaVu12Lat1.h` /
  `DejaVu18Lat1.h`. They keep the original ASCII glyphs byte-identical and add
  German/Latin-1 glyphs (ÄÖÜäöüß, …, U+00A0–U+00FF), stored in LGFX's
  run-length bitmap format. Regenerate with `/tmp/opencode/gen_umlaut_font.py`
  if you ever rebase the font. Non-Latin scripts would need the bundled `efont`
  fonts.
- The buzzer and IMU are not used in v1.
- **Standby power** — the display, backlight expander and RFID stay powered
  in both standby engines, so the board as a whole still draws a fraction of
  a mA to a few mA on top of the chip.  The deep-poll engine only cuts the
  *chip* to ~7 µA; long `STANDBY_POLL_MS` values save the most but make the
  knob/wheel respond lazier (a tap shorter than the interval can be missed —
  hold ~1 s to wake).
- The M5Dial's ESP32-S3-FN8 has no PSRAM; the 240×240 UI frame buffer lives
  in internal RAM and the client caps RAM usage (compact directory listings,
  a capped playlist fetch, filtered `lsinfo` buffering) so large libraries
  never cause an out-of-memory crash.

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

// ---- Time (NTP wall clock for webradio) ------------------------------
// While a webradio stream is playing, the now view shows the local
// wall-clock time instead of the (meaningless) stream play time.
#define NTP_SERVER "pool.ntp.org"
// Time zone offset from UTC in hours (GMT+0 = 0, +1 = 1, +2 = 2, ...).
// Germany: CEST (summer) = 2, CET (winter) = 1.
#define TIMEZONE_UTC_HOURS 2

// ---- UI timeout -------------------------------------------------------
// After this many ms of no input (knob, button or touch) every screen
// except the now-playing view falls back to now playing.  0 disables it.
#define MENU_TIMEOUT_MS    5000   // menu, queue, play menu, instances
#define BROWSE_TIMEOUT_MS  15000  // files / library / playlists (longer, browse takes time)

// ---- Wi-Fi -----------------------------------------------------------
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWD"

// ---- MPD server ------------------------------------------------------
// IP address or hostname of the machine running MPD (port 6600).
#define MPD_HOST "192.168.1.10"
#define MPD_PORT 6600
// Optional MPD password (leave empty if MPD has no password set).
#define MPD_PASSWORD ""

// ---- MPD instances ---------------------------------------------------
// One MPD per room, each on its own port.  Define the port for each room,
// then add both the on-screen name and the define to MPD_INSTANCES below.

// List shown in the room picker: { display name, port }.
// The first entry is the default instance used at boot.
#define MPD_INSTANCES                        \
    { "LivingRoom", 6600 },                  \
    { "Bedroom", 6601 },                     \
    { "Bath", 6602 },                        \
    { "Kitchen", 6603 },                     \
    { "Office", 6604 },                      \
    { "Bar", 6605 },                         \
    { "Basement", 6606 },                    \
    { "AllRooms", 6607 }
```



