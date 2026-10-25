#pragma once

// =====================================================================
//  Hardware backend - M5Stack M5Dial
//
//  Same contract as the CrowPanel backend: the board object `HW` offers
//  display, touch, knob button and encoder, plus the geometry and pin
//  macros main.cpp needs for the standby engines.  See hw.h.
//
//  All device specific details (board init, panel, input) come from
//  M5Unified / the M5Dial library, so the rest of the firmware stays
//  decoupled from the concrete hardware.
// =====================================================================

#include <M5Dial.h>

#define HW_BOARD_NAME "M5Dial"

// ---- panel + logical UI geometry ---------------------------------------
// The UI runs in a square 240x240 sprite on the round 240x240 panel.
#define HW_W        240
#define HW_H        240
#define HW_UI_W     240
#define HW_UI_H     240
#define HW_UI_X     0
#define HW_UI_Y     0
#define HW_ROUND    1

// logical UI centre
#define HW_CX       (HW_UI_W / 2)
#define HW_CY       (HW_UI_H / 2)

// ---- input ------------------------------------------------------------
// M5Dial has the rotary encoder + push-knob; buttons use M5Unified.
#define HW_HAS_ENCODER 1
#define HW_HAS_TOUCH   1

// Pins the standby engines poll directly.  M5Unified owns the button pin
// during normal operation, so these only matter while the chip is dozing.
#define HW_ENC_A      DIAL_ENCODER_PIN_A   // 41
#define HW_ENC_B      DIAL_ENCODER_PIN_B   // 40
#define HW_BTN_PIN    42                   // knob push, active low
#define HW_BTN_LEVEL  0

// ---- board object ------------------------------------------------------
// M5Dial's hardware object already has exactly the API the firmware
// wants, so the abstraction is just a different name for it.
#define HW           M5Dial

// ---- lifecycle hooks used by main.cpp ---------------------------------
static inline void hwBegin() {
    auto cfg = M5.config();
    cfg.fallback_board = m5::board_t::board_M5Dial;  // safety net if probing fails
    M5Dial.begin(cfg, true, false);                   // enable encoder, no RFID
    M5Dial.BtnA.setHoldThresh(600);                   // long-press = mode switch
}

static inline void hwStandbyPollPrepare() {
    pinMode(HW_ENC_A, INPUT_PULLUP);
    pinMode(HW_ENC_B, INPUT_PULLUP);
    pinMode(HW_BTN_PIN, INPUT_PULLUP);
}

// ---- display power -----------------------------------------------------
static inline void hwDisplaySleep() { M5Dial.Lcd.sleep(); }
static inline void hwDisplayWake() { M5Dial.Lcd.wakeup(); }

// ---- status LEDs -------------------------------------------------------
// The M5Dial has no addressable LEDs, so the transport indicator main.cpp
// drives is a no-op here.  Present anyway so main.cpp can call it
// unconditionally instead of guarding every call site with a board check.
static inline void hwLedSetState(bool playing, bool paused, bool wifi, int volume) {
    (void)playing; (void)paused; (void)wifi; (void)volume;
}

// No addressable LEDs, so there is no goodbye animation to play.
static inline uint32_t hwLedShutdown() { return 0; }
