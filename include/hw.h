#pragma once

// =====================================================================
//  Hardware abstraction
//
//  The board is picked by a build flag set in platformio.ini, and the
//  matching backend header supplies:
//    HW                  the board object (display / touch / button /
//                         encoder), shared by main.cpp and ui.cpp
//    hwBegin()           board init, called from setup()
//    hwStandbyPollPrepare()
//                         pin setup for the deep-poll standby engine
//    HW_W / HW_H / HW_UI_* / HW_ROUND / HW_CX / HW_CY
//                         panel + logical UI geometry
//    HW_ENC_A / HW_ENC_B / HW_BTN_PIN / HW_BTN_LEVEL
//                         the three input pins the standby engines poll
//    HW_HAS_ENCODER / HW_HAS_TOUCH
//  Nothing above this header should name a concrete board.
// =====================================================================

#include "config.h"

#if defined(DEVICE_CROWPANEL)
#include "board_crowpanel.h"
#elif defined(DEVICE_M5DIAL)
#include "board_m5dial.h"
#else
#error "no board selected - set DEVICE_M5DIAL or DEVICE_CROWPANEL in platformio.ini"
#endif
