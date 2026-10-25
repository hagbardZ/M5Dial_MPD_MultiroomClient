# Round-knob MPD client (M5Dial / CrowPanel 1.28")
<img width="1408" height="768" alt="MultiroomSchema" src="https://github.com/user-attachments/assets/16a2e74f-1377-4460-9a3c-c772e730fbf1" />
<img width="200" height="200" alt="3" src="https://github.com/user-attachments/assets/2cd0432c-5442-4966-b122-60725384fb41" />
<img width="200" height="200" alt="2" src="https://github.com/user-attachments/assets/8b79b56d-5681-4ea4-b3de-fba0de2117ba" />
<img width="200" height="200" alt="1" src="https://github.com/user-attachments/assets/fde4fbc4-b269-4e55-a1e4-a361631e9343" />
<img width="200" height="200" alt="4" src="https://github.com/user-attachments/assets/02d72d1c-d77d-416a-a742-d769e3e77f7d" />
<img width="200" height="200" alt="5" src="https://github.com/user-attachments/assets/a55250f3-fea4-49c5-9818-92f7de09872b" />
<img width="200" height="200" alt="7" src="https://github.com/user-attachments/assets/eee77671-0258-4008-9d72-ceec5520d0c6" />
<img width="800" height="625" alt="6" src="https://github.com/user-attachments/assets/b68a5cc8-1a68-4d7b-aceb-f51281e3138f" />


A compact [MPD](https://www.musicpd.org/) remote-control client for round
1.28" knob displays, running on either of two boards:

| Board                                                    | SoC / memory                        | Panel                    | Input                        |
|----------------------------------------------------------|-------------------------------------|--------------------------|------------------------------|
| **M5Stack M5Dial** (default env `m5dial`)                | ESP32-S3-FN8, 8 MB flash, no PSRAM  | 240×240 round GC9A01     | rotary encoder + push knob, capacitive touch |
| **Elecrow CrowPanel 1.28"** (env `crowpanel`)            | ESP32-S3R8, 16 MB flash, 8 MB PSRAM | 240×240 round GC9A01     | rotary encoder + push knob, capacitive touch (CST816D) |

Displays the current track and a progress ring, controls playback and volume,
lets you browse the music library, filesystem and stored playlists (M.A.L.P.
style), and reconnects automatically.

## Features

- **Now-playing screen** — artist / title / album, smooth progress ring,
  elapsed + total time, play state, volume.  Long titles scroll (marquee)
  around the round screen.  When Wi-Fi is down the progress ring turns solid
  red (the elapsed arc is hidden until the link is back, so the warning is
  unmissable even with nothing queued).
- **Webradio** — streams show the station name instead of the artist, and
  the real wall-clock time (NTP-synced, timezone offset configurable)
  instead of the meaningless stream play time.
- **Playback control** — play/pause, next, previous, stop, volume, and a
  repeat toggle that always repeats the **current song** (`repeat` + `single`),
  never the whole queue.
- **Main menu** — hold in the now-playing screen for a menu hub:
  Queue, Files, Library, Playlists, Clear playlist, Torch (CrowPanel only),
  Back.
- **Play menu** — tap the "playing" text in the now-playing screen: toggle
  random and repeat-current, plus (with KNX enabled) the first
  `KNX_PLAYMENU_DEVICES` KNX devices as one-tap rows.  Every device row
  shows its live bus state as `[x]` / `[ ]` / `[?]` and toggles its On/Off
  group on a knob click or tap.  A final **KNX** row opens the **KNX
  submenu**, which lists the remaining configured KNX devices (e.g.
  Amplifier 3/4/5 here) in the same style — hold to go back to the play
  menu, or hold again for now-playing.  With `KNX_ENABLE 0` the play menu
  is just random / repeat.
- **KNX (KNXnet/IP tunneling)** — optional, off by default; enable with
  `KNX_ENABLE`.  A minimal UDP tunneling client that talks directly to a KNX
  IP interface (MDT / Gira / TP-UART gateway, default `192.168.23.35:3671`)
  without ETS, and toggles the On/Off (DPT 1.001) group addresses of up to
  five devices — typically amplifiers — split between the play menu (the
  first `KNX_PLAYMENU_DEVICES`) and the KNX submenu (the rest).  Each device
  has a *toggle* group (write `0`/`1`) and a *status* group (read back;
  shown as `[x]`/`[ ]`/`[?]` until it has reported once).  The tunnel sends
  a connection-state request every 30 s to keep the gateway lease alive,
  reconnects with retry if the single tunnelling slot is busy, and never
  blocks the UI.
- **Idle auto-return** — after `MENU_TIMEOUT_MS` without any input, every
  screen other than now playing falls back to it automatically.  The
  file/library/playlist browsers use the longer `BROWSE_TIMEOUT_MS` so you
  have time to look around.
- **Standby** — hold the knob for `STANDBY_PRESS_MS` and the dial sleeps
  (screen + Wi-Fi off); wake by pressing the knob or turning the wheel.  The
  dial also parks itself when Wi-Fi has no link for
  `STANDBY_WIFI_TIMEOUT_MS` (2 min by default), so a missing access point
  costs nothing instead of leaving a red Wi-Fi dot on screen for hours; the
  reconnect attempts keep running during that grace period, so a short
  dropout does not trigger it.  Both triggers are switched off separately in
  `config.h` — see [Standby](#standby).
  Two low-power engines are selectable in `config.h`: *light sleep* for an
  instant wake (~1 mA) or *deep sleep with an RTC-timer poll* (the chip
  idles at ~7 µA, wakes itself every `STANDBY_POLL_MS` to glance at the
  knob/wheel, and dozes again).  The knob/wheel sit on digital-only pins,
  so deep sleep can never be *triggered* directly from them — the poll is
  the trade-off that makes µA-class chip power possible without a wire.
- **LED status ring** (CrowPanel only) — the five addressable pixels around
  the knob report the device state: a **blue** dot circles nine times at boot,
  then the ring follows playback.  While **playing**, a single **green** pixel
  chases round a dark ring and its speed tracks the volume — 2 s per
  revolution when quiet down to 500 ms at full, so the volume is readable
  from across the room.  While **paused**, all five glow **yellow** and
  breathe slowly between off and full; **stopped**, the ring is dark.  With
  no Wi-Fi link the whole ring pulses **red**, which outranks the transport
  state, because a chase or a breath would otherwise imply a healthy device.
  Going to standby plays the same three-turn chase in **red** before the
  panel power is cut.  Selecting **Torch** in the menu holds all five at
  full white and shows `[x] Torch`; selecting it again turns the torch off
  and restores the transport animation.  The M5Dial has no addressable
  LEDs, so the whole feature is absent there.
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
  updates the moment something changes.  If the selected room cannot be
  reached (accepts TCP but never answers — e.g. a wedged MPD), the client
  automatically advances to the next configured instance after a few failed
  attempts, so it never sits on a dead MPD forever unless you explicitly
  picked that room this session.  KNX toggles keep working even while MPD is
  offline.
- **Memory-conscious** — runs in the ~128 KB internal-RAM heap of the
  PSRAM-less ESP32-S3-FN8.  Directory listings are buffered compactly
  (the `lsinfo` response is filtered down to `directory:` / `file:` /
  `playlist:` lines, dropping per-file tag blocks), so even large folders
  fit without OOM.

## Hardware / wiring

No wiring in either case — just power the board via USB-C.

- **M5Dial** — plug in and go.
- **CrowPanel 1.28"** — plug in and go.  The board definition enables both
  panel rails (GPIO1 = `LCD_3V3`, GPIO2 = `LEDA_3V3`) before the display is
  initialised, so no jumper is needed.  If your CrowPanel revision uses the
  4 MB / 8 MB flash variants, adjust `upload.flash_size` and
  `build.partitions` in `boards/elecrow-crowpanel-128.json`.
- **LED ring** (CrowPanel only) — the five WS2812 pixels sit on GPIO48 and
  are fed from the `LEDA_3V3` rail that is already enabled for the display,
  so they need no wiring either.  The pixel count and data pin come from
  `variants/crowpanel_128/pins_arduino.h`.

## Controls

| Input              | Now playing                    | Menu / Queue / Browser        |
|--------------------|--------------------------------|-------------------------------|
| Knob rotate        | Volume                         | Scroll selection              |
| Knob push (short)  | Play / pause                   | Open / enqueue / play         |
| Knob push (2×)     | –                              | Play now / load & play (files, songs, playlists) |
| Knob push (long)   | Open main menu                 | Back (up one level)           |
| Knob push (very long, ≥ `STANDBY_PRESS_MS`) | Standby (sleep)     | Standby (sleep)               |
| Touch              | Prev/next arrows; tap "playing" for the play menu (random / repeat / KNX devices / KNX submenu); tap the title for the queue; tap room name to switch rooms | Tap an entry to select / play; tap a row in the play/KNX menu to toggle it; tap the bottom counter in the file browser to rescan the folder |

On the CrowPanel, turning the volume knob while music plays also changes how
fast the green pixel chases round the ring, so the level can be read off the
LEDs without looking at the screen.

Navigating the browser: rotate to move, click to open a folder / artist /
album, single-click a playlist to clear the queue and play it, single-click a
song or file to add it to the queue, double-click it to play it right away.
Hold anywhere in a browser to go back
one level (and eventually back to the menu; hold in the menu goes back to the
now-playing screen).  In the play menu and KNX submenu, hold to go back one
level too (KNX submenu → play menu → now-playing).

## Build & flash

### Prerequisites

- [PlatformIO Core](https://platformio.org/install) ≥ 6.x (or the PlatformIO
  IDE inside VS Code).
- `git` to clone the repository.

### 1. Get the sources

```bash
git clone <repository-url> MPDclient-M5Dial
cd MPDclient-M5Dial
```

### 2. Create `include/config.h`

`include/config.h` holds your Wi-Fi credentials, the MPD server address and
the optional KNX settings, so it is **not** part of the repository: it is
git-ignored and intentionally left out of the source tree.  The build fails
if it is missing.  Write it from the
[example include/config.h](#example-includeconfigh) below and adjust the
values to match your network.

### 3. Build

```bash
pio run                     # M5Dial (the default environment)
pio run -e crowpanel        # Elecrow CrowPanel 1.28"
```

The first run downloads and installs the ESP32 toolchain, the arduino-esp32
framework and the `m5stack/M5Dial` library automatically; this takes a few
minutes.  Subsequent builds only compile what changed.

### 4. Flash

```bash
pio run -t upload                    # M5Dial over USB
pio run -e crowpanel -t upload       # CrowPanel over USB
```

If the serial port is not auto-detected, uncomment and set `upload_port` in
`platformio.ini` (e.g. `upload_port = /dev/ttyACM0`).

On Linux the automatic chip reset over the native USB can fail with
`OSError: [Errno 5] Input/output error`: esptool drops DTR, the ESP32-S3
resets, its USB disappears and the upload dies on the next ioctl.  Hold **BOOT**
(GPIO0) while tapping **RESET** to enter the ROM downloader, then flash with the
reset dance disabled — copy the esptool line from `pio run -t upload -v` and add
`--before no_reset --after no_reset`.

### 5. Serial log

```bash
pio device monitor      # serial log at 115200 baud
```

### Other useful commands

```bash
pio run -t clean        # full rebuild (delete build artifacts)
pio run -v              # verbose output for debugging build issues
pio run -t upload && pio device monitor   # flash and follow the log
```

### Build notes

- The board is picked by a build flag: `-DDEVICE_M5DIAL` for the `m5dial`
  environment, `-DDEVICE_CROWPANEL` for the `crowpanel` environment.  Neither
  flag is set by accident, so `include/hw.h` fails the build instead of
  silently targeting the wrong board.
- **M5Dial** — the board definition lives in `boards/m5stack-dial.json`
  (mirrors the official Arduino `M5Stack Dial` board).  The module is the
  **FN8** variant — 8 MB flash and **no PSRAM** — so the firmware is tuned
  for the ~128 KB internal-RAM heap and never relies on PSRAM.  The pinned
  arduino-esp32 fork does not ship the `m5stack_dial` pin map, so it is
  provided project-locally in `variants/m5stack_dial/pins_arduino.h` and
  wired in via the board's `build.variants_dir`.
- **CrowPanel 1.28"** — the board definition lives in
  `boards/elecrow-crowpanel-128.json` with the pin map in
  `variants/crowpanel_128/pins_arduino.h`.  M5Unified has no entry for this
  board, so `include/board_crowpanel.h` drives M5GFX directly: it wires
  `Panel_GC9A01` (SPI), `Touch_CST816D` (I2C, address `0x15`) and a PWM
  backlight onto the two panel rails by itself, then adds the same
  `m5::Button_Class` used by the M5Dial backend.  The knob encoder is
  decoded locally rather than with the M5Dial `Encoder` class: the PJRC
  version only attaches interrupts to a fixed pin list (GPIO 0-5, 12-18,
  21-23, 25-27, 32-36, 39) and the CrowPanel knob is on GPIO 45/42, so it
  silently degrades to sampling both pins once per `read()` — far too slowly
  next to the UI redraw, which makes the count alias and the knob appear to
  jump or run backwards.  The replacement samples both channels every 1 ms
  on a dedicated task and debounces each one independently: a channel only
  changes its stable level after holding the new level for four consecutive
  samples, so contact ring never reaches the quadrature accumulator.  Only
  stable changes are counted, with full-quadrature cancellation, rest-state
  resync and diagonal momentum recovery; four net transitions are one
  detent.  Flash
  and PSRAM are configured for the ESP32-S3R8 (16 MB flash, **OPI** PSRAM,
  `memory_type = qio_opi`, `psram_type = opi`) exactly as Elecrow's Arduino
  instructions do; the UI frame buffer then lives in PSRAM.
- Version-pin `platform = espressif32@7.1.3` in `platformio.ini` if you need
  a reproducible build.

## Configuration

`include/config.h` is **not** shipped in the source tree.  You must create it
yourself before building — copy the full
[example include/config.h](#example-includeconfigh) below into
`include/config.h` and edit it:

```c
#define WIFI_SSID      "myssid"        // your 2.4 GHz SSID
#define WIFI_PASS      "mypassword"
#define MPD_HOST       "192.168.1.10"  // IP or hostname of the MPD machine
#define MPD_PORT       6600            // default instance port
#define MPD_PASSWORD   ""              // optional MPD password

// --- KNX (optional KNXnet/IP tunneling for amplifier on/off) ---
#define KNX_ENABLE       1      // 0 = off (device rows hidden in play menu)
#define KNX_HOST         "192.168.23.35"  // KNX IP interface (tunnelling)
#define KNX_PORT         3671             // KNXnet/IP port
#define KNX_LOCAL_PORT   3672             // local UDP source port (0 = auto)
#define KNX_MY_ADDRESS   0xFFFA           // own source address, fresh per device
#define KNX_DEBUG        1                // 1 = verbose serial log
#define KNX_DEVICE1_NAME "Amplifier 1"
#define KNX_DEVICE1_TOGGLE_MAIN   2    // toggle On/Off group address
#define KNX_DEVICE1_TOGGLE_MIDDLE 1    // (main, middle, sub)
#define KNX_DEVICE1_TOGGLE_SUB    1
#define KNX_DEVICE1_STATUS_MAIN   2    // status group, read back on boot
#define KNX_DEVICE1_STATUS_MIDDLE 1
#define KNX_DEVICE1_STATUS_SUB    0
// ... and the analogous KNX_DEVICE2_* .. KNX_DEVICE5_*  for the other
//     devices (2, 4, 5 sit in the KNX submenu via KNX_PLAYMENU_DEVICES).
#define KNX_PLAYMENU_DEVICES 2         // # of devices pinned to the play menu

#define NTP_SERVER         "pool.ntp.org"  // NTP for the webradio wall clock
#define TIMEZONE_UTC_HOURS 2              // GMT offset (Germany: CEST=2, CET=1)
#define MENU_TIMEOUT_MS    5000           // idle auto-return (menu/queue/play menu); 0 = off
#define BROWSE_TIMEOUT_MS  15000          // idle auto-return while browsing files/library/playlists; 0 = off

// --- Standby ---
#define STANDBY_PRESS_MS  2000   // very-long knob hold → standby; 0 = off
#define STANDBY_WIFI_TIMEOUT_MS 120000  // no Wi-Fi link this long → standby; 0 = off
#define STANDBY_DEEP_POLL 1      // 1 = deep sleep + RTC-timer poll, 0 = light sleep (instant wake)
#define STANDBY_POLL_MS   1000   // knob/wheel scan interval in deep-poll mode
```

MPD must be listening on TCP 6600 on your network (`bind_to_address` in
`mpd.conf`, e.g. `"any"` for LAN access). No extra MPD plugins required.

Rooms are defined in `MPD_INSTANCES` (one MPD process per port). The first
entry is used at boot; switch rooms by tapping the room name shown at the top
of the now-playing screen (`MODE_INSTS`).  If the selected room cannot be
reached (connects but never sends its greeting), the client hops to the next
instance automatically after a few failures.

### Standby

Two independent triggers, both configured in `config.h`:

| Trigger | Define | Default | What it does |
|---------|--------|---------|--------------|
| Very-long knob hold | `STANDBY_PRESS_MS` | `2000` | Screen + Wi-Fi off, sleep until the knob is pressed or the wheel is turned.  `0` = off. |
| No Wi-Fi link | `STANDBY_WIFI_TIMEOUT_MS` | `120000` (2 min) | The same sleep, entered automatically.  `0` = off. |

The Wi-Fi trigger is a link watchdog in `loop()` (`checkWifiStandby()` in
`src/main.cpp`): the network task keeps its 15 s reconnect attempts, so the
dial first shows the red ring and only dozes off once the outage lasts
`STANDBY_WIFI_TIMEOUT_MS` — a missing access point costs nothing instead of
leaving a red dot on screen for hours.  A single `WL_CONNECTED` restarts the
timer, and a dial woken while the AP is still gone gets the full timeout again
before sleeping once more.  Both paths use the same sleep engine, so
`STANDBY_DEEP_POLL` / `STANDBY_POLL_MS` apply to the automatic sleep as well.
In the serial log:

```
[standby] no Wi-Fi link for 120000 ms, going to standby
[standby] deep sleep, scanning knob/wheel every 1000 ms
```

Because the two triggers are separate defines, you can keep one and drop the
other — e.g. `STANDBY_PRESS_MS 0` for a dial nobody should be able to put to
sleep by accident, which still parks itself when the AP disappears.  With both
at `0` the whole standby code is compiled out.

## Notes

- The bundled M5GFX DejaVu fonts are ASCII-only (no umlauts), so the project
  ships umlaut-capable variants in `src/fonts/` — `DejaVu12Lat1.h` /
  `DejaVu18Lat1.h`. They keep the original ASCII glyphs byte-identical and add
  German/Latin-1 glyphs (ÄÖÜäöüß, …, U+00A0–U+00FF), stored in LGFX's
  run-length bitmap format. Regenerate with `/tmp/opencode/gen_umlaut_font.py`
  if you ever rebase the font. Non-Latin scripts would need the bundled `efont`
  fonts.
- **KNX specifics** — the (optional) KNX client is a pure KNXnet/IP
  tunneling over UDP; it needs no ETS and no group-address table since the
  toggle/status groups are hard-coded in `config.h`.  Toggling sends a
  *write* to the toggle group, then the device's own status group echoes the
  new state back (`[x]`/`[ ]` updates) — the two addresses just need to be
  wired in ETS as *On/Off* (DPT 1.001).  Give the Dial a dedicated source
  address (`KNX_MY_ADDRESS`) per free KNX interface slot; devices each
  need their own free tunnelling slot, and ETS/monitoring tools claiming the
  slot show up as a refused connection.
- **LED ring details** (CrowPanel only) — the strip is driven through RMT
  rather than bit-banged, because a bit-banged frame that gets preempted part
  way through corrupts every pixel after the interruption, and this firmware
  has plenty to preempt it: Wi-Fi, MPD, KNX and the UI all run tasks.  RMT
  builds the frame in a DMA-capable buffer and the hardware clocks it out
  with no CPU involvement.  The pinned ESP-IDF (4.4) predates
  `RMT_DEFAULT_CONFIG()` and the modern `led_strip` component is not linked,
  so the channel is configured by hand against the legacy driver, and the
  item buffer comes from `heap_caps_malloc(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`
  — a plain `malloc()` may return PSRAM, which the RMT interrupt context
  cannot read, and the symptom is not a crash but a corrupted bitstream, so
  the pixels come out the wrong colour.  WS2812 is wired GRB, so the frame
  has to be written byte 0 first, MSB first: a single 23..0 bit counter
  reads the colour array backwards and shows a green pixel as blue, because
  the strip treats the first byte it receives as green.
- The buzzer and IMU are not used in v1.
- **Standby power** — the ~7 µA figure is the *chip* in deep sleep, measured at
  its VDD pins.  A whole board does not read that: what you measure is
  dominated by everything around the chip.
  - The **poll interval** is the term firmware can actually control, and it is
    a duty-cycle effect rather than a leak.  The chip sleeps at ~7 µA, but each
    poll is a full boot drawing tens of mA, so the average is roughly
    `boot_ms × boot_mA / STANDBY_POLL_MS`.  Measured on the CrowPanel: 1000 ms
    gave 3–11 mA, 15000 ms gave 2.8 mA.  So the interval is worth 1–8 mA and
    there is a ~2.8 mA floor underneath that firmware cannot reach.  The
    default stays at 1000 ms because a knob that responds within a second is
    worth more than the mA; set 15000 if battery life matters more.  The knob
    and wheel are on GPIO 45/42/41 and the ESP32-S3 has RTC GPIOs only on
    0–21, so no `ext0`/`ext1` wake is possible and polling is the only option
    on this board.
  - What firmware **cannot** reach: the onboard LDO's quiescent current
    (included if you probe the 5 V input rather than 3V3), a power indicator
    LED, and anything still powered from a rail that is never switched.
    Probing 3V3 as well as 5 V separates the regulator from the loads.  Long
    `STANDBY_POLL_MS` values save the most but make the knob/wheel respond
    lazier — a tap shorter than the interval is missed, so hold until it wakes,
    up to `STANDBY_POLL_MS`.
  - *Light sleep* is the alternative when instant wake matters more than
    current: it skips the boot entirely but sits at ~0.7–1.5 mA steadily.
- The M5Dial's ESP32-S3-FN8 has no PSRAM; the 240×240 UI frame buffer lives
  in internal RAM and the client caps RAM usage (compact directory listings,
  a capped playlist fetch, filtered `lsinfo` buffering) so large libraries
  never cause an out-of-memory crash.  The CrowPanel has 8 MB of OPI PSRAM, so
  the same sprite is allocated there; the memory-conservative code paths are
  kept in place anyway so one UI serves both boards.

## Project layout

```
boards/        custom board definitions (PlatformIO): m5stack-dial + elecrow-crowpanel-128
include/       config.h (not in the repo — create it from the example below)
               hw.h                    -> the board backend chosen by the build flag
               board_m5dial.h          -> M5Dial backend (wraps M5Dial)
               board_crowpanel.h       -> CrowPanel backend (M5GFX + button, encoder,
                                          WS2812 LED ring) and their shared statics
variants/      project-local pin maps (m5stack_dial, crowpanel_128)
lib/MpdClient/ MPD protocol client + PSRAM playlist parser
src/           main, background MPD task, round-screen UI, KNX tunneling client
               hw.cpp -> storage for the process-wide singletons the board headers
                         declare (encoder registers, RMT channel, LED buffers)
```

### example include/config.h:


```
#pragma once

// =====================================================================
//  Round-knob MPD client - configuration
//
//  Adjust the values below to match your network and MPD setup, then
//  build with `pio run` and flash with `pio run -t upload`.
// =====================================================================

// ---- Target hardware ----------------------------------------------------
// Selected by the build flag in platformio.ini:
//   m5dial     -> DEVICE_M5DIAL      (M5Stack M5Dial)
//   crowpanel  -> DEVICE_CROWPANEL   (Elecrow CrowPanel 1.28")
// Exactly one of them must be set; hw.h pulls in the matching backend and
// fails the build otherwise.

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

// ---- Standby -------------------------------------------------------
// 1 = deep sleep + RTC-timer poll (~7 uA, wakes every STANDBY_POLL_MS to
// scan the knob/wheel), 0 = light sleep (instant wake, ~1 mA).
#define STANDBY_DEEP_POLL  1
#define STANDBY_POLL_MS    1000   // knob/wheel scan interval in deep-poll mode
// Very-long knob hold -> standby; 0 = off (the timeout below still works).
#define STANDBY_PRESS_MS   2000
// No Wi-Fi link for this long -> standby; 0 = off.
#define STANDBY_WIFI_TIMEOUT_MS 120000   // 2 min

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

// ---- KNX (optional) ---------------------------------------------------
// KNXnet/IP tunnelling client: toggles On/Off group addresses of up to five
// devices (e.g. amplifiers).  The first KNX_PLAYMENU_DEVICES devices appear
// as one-tap rows in the play menu; the rest live in the KNX submenu.  Set
// KNX_ENABLE 0 to leave it out (the play menu then only shows random +
// repeat).
#define KNX_ENABLE     1                       // 0 = off
#define KNX_HOST       "192.168.23.35"         // tunnelling interface (MDT/Gira/TP-UART)
#define KNX_PORT       3671
#define KNX_LOCAL_PORT 3672                    // local UDP source port (0 = auto)
#define KNX_MY_ADDRESS 0xFFFA                  // own source address (per device)
#define KNX_DEBUG      1                       // 1 = verbose serial log
#define KNX_PLAYMENU_DEVICES 2                 // # pinned to the play menu

// Device 1 (play menu): toggle + status group (2/1/1 and 2/1/0).
#define KNX_DEVICE1_NAME "Amplifier 1"
#define KNX_DEVICE1_TOGGLE_MAIN   2
#define KNX_DEVICE1_TOGGLE_MIDDLE 1
#define KNX_DEVICE1_TOGGLE_SUB    1
#define KNX_DEVICE1_STATUS_MAIN   2
#define KNX_DEVICE1_STATUS_MIDDLE 1
#define KNX_DEVICE1_STATUS_SUB    0

// Device 2 (play menu): toggle + status group (2/0/0 and 2/0/1).
#define KNX_DEVICE2_NAME "Amplifier 2"
#define KNX_DEVICE2_TOGGLE_MAIN   2
#define KNX_DEVICE2_TOGGLE_MIDDLE 0
#define KNX_DEVICE2_TOGGLE_SUB    0
#define KNX_DEVICE2_STATUS_MAIN   2
#define KNX_DEVICE2_STATUS_MIDDLE 0
#define KNX_DEVICE2_STATUS_SUB    1

// Devices 3..5 (KNX submenu): same pattern, e.g. amplifier 3 uses 2/0/2 +
// 2/0/3, amplifier 4 uses 2/0/4 + 2/0/5, amplifier 5 uses 2/0/6 + 2/0/7 —
// adjust the group addresses to your bus.
#define KNX_DEVICE3_NAME "Amplifier 3"
#define KNX_DEVICE3_TOGGLE_MAIN   2
#define KNX_DEVICE3_TOGGLE_MIDDLE 0
#define KNX_DEVICE3_TOGGLE_SUB    2
#define KNX_DEVICE3_STATUS_MAIN   2
#define KNX_DEVICE3_STATUS_MIDDLE 0
#define KNX_DEVICE3_STATUS_SUB    3
// ... KNX_DEVICE4_* / KNX_DEVICE5_* analogous
```



