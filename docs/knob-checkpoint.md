# CrowPanel knob debugging checkpoint

Resume by saying: "resume the knob checkpoint".

## Status: resolved (2026-10-25)

The debounced sampling decoder is implemented, tested on hardware and
flashed as the release build. Test: 19 slow clicks plus a fast spin gave
26 detent events, every one `d=+1`, with `rst=0`, `b=0` and idle `acc=0`.
No phantom steps. Debug logging has been removed from the release build.

## Objective

Fix the rotary knob on the CrowPanel 1.28 (ESP32-S3, env `crowpanel`,
`/dev/ttyACM3`, now often `/dev/ttyACM0`) in Now Playing / volume mode.
Original symptoms: missed steps and inconsistent volume changes, later
diagnosed as occasional double-counting (phantom steps) during slow turns.

## Diagnosis (confirmed)

The encoder contacts ring heavily while moving. Quadrature cancellation
usually holds, but the accumulator random-walks to a phantom detent.

Evidence from `/tmp/knob_capture3.log`:

- At rest: `edges=0` from t=15 s to t=21 s (`/tmp/idle_cap.log`).
- While turning: 234-412 raw edges per second while producing only
  1-4 detents per second.
- `pos 2 -> 3 -> 4` within 12 ms for one real click.
- `pos 9 -> 8 -> 9` around one real click: phantom -1 then real +1,
  i.e. the value appears not to change.
- `th=counted` and `rst=0` throughout, so rest resync is not the cause.

Root cause: contact bounce, not the quadrature math, Wi-Fi, or CPU load.
No idle noise is present.

## Current code state

- Decoder is the per-channel debounce sampled from `KnobEncoder::poll()`
  at 1 ms by `encTask` in `src/hw.cpp`:
  - `CROWPANEL_KNOB_DEBOUNCE_SAMPLES` (4) in `include/board_crowpanel.h`.
  - Only stable-state changes feed the quadrature accumulator, rest
    resync and diagonal momentum recovery.
  - No interrupts, no `IRAM_ATTR`.
- A diagonal with no known direction (`s_lastDir == 0`) is dropped but
  `s_prev` adopts the new state, so a polling loop cannot re-see it.
- Release firmware is flashed: `-DVOL_DEBUG=1` and the 1 Hz `[enc] tick`
  log are removed.
- Volume path fix is working: `mpdAddVolumeDelta()` accumulates into
  `std::atomic<int32_t> s_volPending`, and `drainCommands()` aggregates
  it with queued `CMD_SET_VOL`.
- Capture tooling: `/tmp/capture.py` (pyserial, non-buffered, writes each
  chunk). Recreate it if `/tmp` was cleared. Do not use plain `cat`: it
  buffers and drops output on SIGTERM.
- Test log: `/tmp/knob_debounce.log`.

## Original planned change (done)

Replace the ISR decoder with per-channel debounce sampled from `poll()`
at 1 ms:

- Each channel must remain stable for `KNOB_DEBOUNCE_SAMPLES` before its
  stable level changes (started with 4).
- Feed only stable-state changes into the existing quadrature
  accumulator, rest resync, and diagonal momentum recovery.
- Remove `attachInterrupt` and `isr()` and the `IRAM_ATTR` machinery.
- Move `poll()` from inline-in-header to an out-of-line definition in
  `src/hw.cpp`.
- Retest 20 slow clicks, then a fast turn. Done.
- Afterwards remove `-DVOL_DEBUG=1` and the temporary tick logging. Done.

## Commands

    pio run -e crowpanel
    pio run -e crowpanel -t upload --upload-port /dev/ttyACM0
    python3 /tmp/capture.py /dev/ttyACM0 45 /tmp/knob_debounce.log

The ACM number depends on enumeration; check `ls /dev/ttyACM*` first.
Close any capture before uploading or the port is busy.

## Relevant files

- `include/board_crowpanel.h`: `KnobEncoder` class, debounce constant,
  state, stats.
- `src/hw.cpp`: encoder statics, `poll()`, `encTask`, `begin()`.
- `src/ui.cpp`: `uiEncoder()` Now Playing volume path, `[vol] ui` log.
- `src/mpd_task.cpp` / `src/mpd_task.h`: `s_volPending`,
  `mpdAddVolumeDelta()`, `drainCommands()`.
- `platformio.ini`: temporary `-DVOL_DEBUG=1`.

## Repository state

Untracked: `boards/elecrow-crowpanel-128.json`,
`include/board_crowpanel.h`, `include/board_m5dial.h`, `src/hw.cpp`,
`variants/crowpanel_128/`, this file.

Modified tracked: `platformio.ini`, `src/ui.cpp`, `src/mpd_task.cpp`,
`src/mpd_task.h`, plus pre-existing changes in `.codex/config.toml`,
`README.md`, `include/hw.h`, `src/main.cpp`.

Do not commit without explicit request.
