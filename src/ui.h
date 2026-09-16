#pragma once

#include <stdint.h>

void uiInit();                 // setup sprite/screen
void uiTick();                 // render (call every loop iteration)

// input events (push from the main loop)
void uiEncoder(int delta);     // knob rotation
void uiButtonClick();          // knob push (short)
void uiButtonHold();           // knob push (long)
void uiButtonDecide();         // single/double-click decided (call every loop)
void uiTouchTick();            // screen touch (prev/next arrows in now view)