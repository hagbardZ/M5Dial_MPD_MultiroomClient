#pragma once

// =====================================================================
//  Hardware abstraction - M5Dial
//
//  All device-specific details (board init, display geometry, input)
//  flow through the HW_* macros and the shared M5Unified API
//  (M5.begin / M5.Display / M5.Touch / M5.BtnA), so the rest of the
//  firmware stays decoupled from the concrete hardware.
// =====================================================================

#include "config.h"

// ---- panel + logical UI geometry -------------------------------------
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
#define HW_HAS_ENCODER   1
#define HW_BTN_A         M5.BtnA   // knob push
#define HW_BTN_B         M5.BtnB   // (reserved)

// ---- M5 stack ---------------------------------------------------------
#include <M5Dial.h>