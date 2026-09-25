#include <Arduino.h>
#include <M5Dial.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>

#include "app.h"
#include "config.h"
#include "mpd_task.h"
#include "ui.h"

// ---------------------------------------------------------------------
static long s_lastEnc = 0;

// Standby exists as soon as one of its two triggers is compiled in: the
// long knob press and the Wi-Fi-offline watchdog.  Either can be switched
// off on its own in config.h.
#define STANDBY_ENABLED \
    (STANDBY_PRESS_MS > 0 || STANDBY_WIFI_TIMEOUT_MS > 0)

#if STANDBY_ENABLED
#if STANDBY_DEEP_POLL && STANDBY_POLL_MS > 0
#define STANDBY_USE_DEEP 1   // deep sleep + RTC-timer poll (default engine)
#else
#define STANDBY_USE_DEEP 0   // light sleep, instant knob/wheel wake
#endif

#define STANDBY_BTN   ((gpio_num_t)42)              // knob push (active low)
#define STANDBY_ENC_A ((gpio_num_t)DIAL_ENCODER_PIN_A)  // wheel channel A (41)
#define STANDBY_ENC_B ((gpio_num_t)DIAL_ENCODER_PIN_B)  // wheel channel B (40)

// ---- shared helpers -------------------------------------------------
static void waitForButtonRelease(uint32_t maxMs) {
    uint32_t t0 = millis();
    while (M5Dial.BtnA.isPressed() && millis() - t0 < maxMs) {
        M5Dial.update();
        delay(5);
    }
}

// Consume any leftover button events from the wake press and drop the
// wake-time wheel rotation so the device does not jump straight into a
// play/pause toggle or a volume step.
static void swallowWakeInput() {
    uint32_t t0 = millis();
    bool sawPress = false;
    while (millis() - t0 < 600) {
        M5Dial.update();
        if (M5Dial.BtnA.isPressed()) sawPress = true;
        else if (sawPress) break;               // the wake press was released
        delay(5);
    }
    M5Dial.BtnA.wasClicked();
    M5Dial.BtnA.wasHold();
    M5Dial.BtnA.wasDoubleClicked();
    M5Dial.BtnA.setState(millis(),
                         m5::Button_Class::button_state_t::state_nochange);
    s_lastEnc = M5Dial.Encoder.read();
}

// ---- quadrature helper for the raw wheel pins -----------------------
static uint8_t encStatePins() {
    return (uint8_t)((digitalRead(STANDBY_ENC_A) << 1) |
                      digitalRead(STANDBY_ENC_B));
}

static int8_t encStepPins(uint8_t prev, uint8_t now) {
    static const int8_t tbl[16] = {
        0, -1,  1, 0,
        1,  0,  0, -1,
       -1,  0,  0, 1,
        0,  1, -1, 0,
    };
    return tbl[((prev & 3) << 2) | (now & 3)];
}

// ---- deep-poll engine ------------------------------------------------
// All three input pins are digital-only (no RTC wakeup), so we cannot
// trigger a deep-sleep wake from them.  Instead we sleep in deep sleep
// (~7 uA) and use the RTC timer to wake ourselves every STANDBY_POLL_MS,
// glance at the knob/wheel, and go back to sleep.  The state that must
// survive the sleep (wake request, wheel steps turned while asleep)
// lives in RTC memory, which survives deep sleep.
RTC_DATA_ATTR static bool     s_rtcArm;         // deep-poll standby armed
RTC_DATA_ATTR static uint8_t  s_rtcPrev;        // last wheel gray code
RTC_DATA_ATTR static int32_t  s_rtcSteps;       // wheel steps during standby
RTC_DATA_ATTR static uint32_t s_rtcMaxPollUs;   // worst poll overhead seen

// Called from setup() on a timer-wake while the poll is armed.  Must be
// cheap and must not touch Serial / M5Dial / Wi-Fi: it only re-reads the
// three pins.  Returns true when the knob was pressed or the wheel moved.
static bool pollStandbyWake() {
    uint32_t t0 = esp_timer_get_time();
    pinMode(STANDBY_ENC_A, INPUT_PULLUP);
    pinMode(STANDBY_ENC_B, INPUT_PULLUP);
    pinMode(STANDBY_BTN,  INPUT_PULLUP);

    uint8_t now = encStatePins();
    bool moved = (now != s_rtcPrev);
    if (moved) {
        s_rtcSteps += encStepPins(s_rtcPrev, now);
        s_rtcPrev = now;
    }

    bool press = (digitalRead(STANDBY_BTN) == 0);
    if (press) {
        esp_rom_delay_us(5000);                  // cheap debounce
        press = (digitalRead(STANDBY_BTN) == 0);
    }

    uint32_t dt = esp_timer_get_time() - t0;
    if (dt > s_rtcMaxPollUs) s_rtcMaxPollUs = dt;
    return press || moved;
}

// ---- light-sleep engine (fallback, instant wake) --------------------
static void enableStandbyWakeup() {
    for (auto p : { STANDBY_BTN, STANDBY_ENC_A, STANDBY_ENC_B })
        gpio_wakeup_enable(p, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}
#endif  // STANDBY_ENABLED

// ---- standby entry ---------------------------------------------------
#if STANDBY_ENABLED
static void enterStandby() {
    Serial.println("[standby] entering standby (press knob or wheel to wake)");
    M5.Lcd.sleep();                             // backlight off + panel sleep
    waitForButtonRelease(3000);                 // do not sleep while held down
    mpdSetStandby(true);                        // freeze the network task
    WiFi.mode(WIFI_OFF);
    delay(250);                                 // let the Wi-Fi stack wind down

#if STANDBY_USE_DEEP
    s_rtcArm   = true;
    s_rtcPrev  = encStatePins();
    s_rtcSteps = 0;
    esp_sleep_enable_timer_wakeup((uint64_t)STANDBY_POLL_MS * 1000);
    Serial.printf("[standby] deep sleep, scanning knob/wheel every %u ms\n",
                  (unsigned)STANDBY_POLL_MS);
    esp_deep_sleep_start();                     // chip resets, setup() re-runs
    Serial.println("[standby] ERROR: deep sleep did not start!");
    esp_restart();                              // should never be reached
#else
    enableStandbyWakeup();
    esp_light_sleep_start();                    // blocks until a wakeup

    // ---- woke up in place (button or wheel) ----
    WiFi.mode(WIFI_STA);                        // the task reconnects on its own
    mpdSetStandby(false);
    M5.Lcd.wakeup();
    swallowWakeInput();
    Serial.println("[standby] woken, resuming");
#endif
}

// ---- Wi-Fi offline watchdog ------------------------------------------
// The network task keeps retrying the access point every 15 s, so a missing
// AP does not stall anything - but there is no point in burning battery on
// a red Wi-Fi dot for hours either.  If there has been no link for
// STANDBY_WIFI_TIMEOUT_MS the device dozes off like a manual standby.
// Only a real link restarts the timer; a reconnect attempt does not.
#if STANDBY_WIFI_TIMEOUT_MS > 0
static void checkWifiStandby() {
    static bool     s_offline = false;
    static uint32_t s_since   = 0;

    if (WiFi.status() == WL_CONNECTED) {
        s_offline = false;
        return;
    }
    if (!s_offline) {              // first sighting of the outage
        s_offline = true;
        s_since   = millis();
        return;
    }
    if ((uint32_t)(millis() - s_since) < (uint32_t)STANDBY_WIFI_TIMEOUT_MS)
        return;

    Serial.printf("[standby] no Wi-Fi link for %u ms, going to standby\n",
                  (unsigned)STANDBY_WIFI_TIMEOUT_MS);
    s_offline = false;             // a woken device gets a fresh timeout
    enterStandby();
}
#endif  // STANDBY_WIFI_TIMEOUT_MS > 0
#endif  // STANDBY_ENABLED

void setup() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

#if STANDBY_ENABLED && STANDBY_USE_DEEP
    // A timer wake while the deep-poll standby is armed: look at the
    // knob/wheel once, and if nothing happened, fall straight back into
    // deep sleep without ever initialising the display / Wi-Fi / UI.
    if (cause == ESP_SLEEP_WAKEUP_TIMER && s_rtcArm) {
        bool wake = pollStandbyWake();
        if (!wake) {
            esp_sleep_enable_timer_wakeup((uint64_t)STANDBY_POLL_MS * 1000);
            esp_deep_sleep_start();
            for (;;) { }                        // never reached
        }
        s_rtcArm = false;                       // real wake: boot normally
    }
#endif

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

#if STANDBY_ENABLED && STANDBY_USE_DEEP
    if (s_rtcSteps) {
        Serial.printf("[standby] woken, %d wheel step(s) while asleep "
                      "(worst poll overhead %.2f ms)\n",
                      (int)s_rtcSteps, s_rtcMaxPollUs / 1000.0);
        swallowWakeInput();                     // drain the wake button press
        uiEncoder((int)s_rtcSteps);             // apply the standby rotation
        s_rtcSteps = 0;
    }
#endif
}

void loop() {
    M5Dial.update();

    // ---- long knob press = standby -------------------------------------
#if STANDBY_PRESS_MS > 0
    static bool s_standbyArmed = false;
    if (M5Dial.BtnA.isPressed()) {
        if (!s_standbyArmed && M5Dial.BtnA.pressedFor(STANDBY_PRESS_MS)) {
            s_standbyArmed = true;
            enterStandby();
        }
    } else {
        s_standbyArmed = false;
    }
#endif

    // ---- no Wi-Fi for a while = standby --------------------------------
#if STANDBY_WIFI_TIMEOUT_MS > 0
    checkWifiStandby();
#endif

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