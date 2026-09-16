#include <Arduino.h>
#include <M5Dial.h>

#include "app.h"
#include "config.h"
#include "mpd_task.h"
#include "ui.h"

// ---------------------------------------------------------------------
static long s_lastEnc = 0;

void setup() {
    auto cfg = M5.config();
    cfg.fallback_board = m5::board_t::board_M5Dial;  // safety net if probing fails
    M5Dial.begin(cfg, true, false);  // enable encoder, no RFID

    Serial.begin(115200);
    Serial.printf("M5Dial MPD client - connecting to %s:%d\n", MPD_HOST,
                  MPD_PORT);

    gShared.mux = xSemaphoreCreateMutex();
    gCmdQueue   = xQueueCreate(12, sizeof(MpdCommand));

    M5Dial.BtnA.setHoldThresh(600);  // long-press = mode switch

    uiInit();
    startMpdTask();
}

void loop() {
    M5Dial.update();

    // ---- rotary encoder (volume / playlist scroll) -------------------
    long e = M5Dial.Encoder.read();
    long d = e - s_lastEnc;
    if (d != 0) {
        s_lastEnc = e;
        uiEncoder((int)d);
    }

    // ---- knob push ----------------------------------------------------
    if (M5Dial.BtnA.wasClicked()) uiButtonClick();
    if (M5Dial.BtnA.wasHold()) uiButtonHold();
    uiButtonDecide();  // single/double-click decision (browser)
    uiTouchTick();     // prev/next touch arrows in now view

    uiTick();
    delay(4);
}