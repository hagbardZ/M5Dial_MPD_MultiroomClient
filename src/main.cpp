#include <Arduino.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>

#include "app.h"
#include "config.h"
#include "hw.h"
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

// How long a knob press may be held down while entering standby before we
// stop waiting for the release.  Generous enough to cover a deliberate
// long press, but bounded so a stuck or bouncing switch cannot block the
// shutdown forever.
#if STANDBY_DEEP_POLL && STANDBY_POLL_MS > 5000
#define WAKE_HOLD_MAX_MS (STANDBY_POLL_MS)
#else
#define WAKE_HOLD_MAX_MS 3000
#endif

#define STANDBY_BTN   ((gpio_num_t)HW_BTN_PIN)        // knob push
#define STANDBY_ENC_A ((gpio_num_t)HW_ENC_A)          // wheel channel A
#define STANDBY_ENC_B ((gpio_num_t)HW_ENC_B)          // wheel channel B

// ---- shared helpers -------------------------------------------------
static void waitForButtonRelease(uint32_t maxMs) {
    uint32_t t0 = millis();
    while (HW.BtnA.isPressed() && millis() - t0 < maxMs) {
        HW.update();
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
        HW.update();
        if (HW.BtnA.isPressed()) sawPress = true;
        else if (sawPress) break;               // the wake press was released
        delay(5);
    }
    HW.BtnA.wasClicked();
    HW.BtnA.wasHold();
    HW.BtnA.wasDoubleClicked();
    HW.BtnA.setState(millis(),
                     m5::Button_Class::button_state_t::state_nochange);
    s_lastEnc = HW.Encoder.read();
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
// cheap and must not touch Serial / the board object / Wi-Fi: it only
// re-reads the three pins.  Returns true when the knob was pressed or
// the wheel moved.
static bool pollStandbyWake() {
    uint32_t t0 = esp_timer_get_time();
    hwStandbyPollPrepare();

    uint8_t now = encStatePins();
    bool moved = (now != s_rtcPrev);
    if (moved) {
        s_rtcSteps += encStepPins(s_rtcPrev, now);
        s_rtcPrev = now;
    }

    bool press = (digitalRead(STANDBY_BTN) == HW_BTN_LEVEL);
    if (press) {
        esp_rom_delay_us(5000);                  // cheap debounce
        press = (digitalRead(STANDBY_BTN) == HW_BTN_LEVEL);
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

    // The goodbye animation has to run before hwDisplaySleep() below, which
    // drops the LED power rail.  Once the rail is down the pixels are dead,
    // so anything the LED task still wants to draw afterwards is lost.  The
    // call is blocking and takes about 1.5 s for the three turns.
    const uint32_t ledMs = hwLedShutdown();
    if (ledMs) Serial.printf("[standby] LED goodbye took %u ms\n", (unsigned)ledMs);

    hwDisplaySleep();                           // backlight off + panel sleep
    // Do not sleep while the knob is still held: the next poll would read
    // that held state as a fresh press and wake the device right back up.
    // The bound has to cover a long press, which is what the user is doing
    // to get here, so it is tied to the poll interval rather than a fixed
    // 3 s - with a 15 s poll a 3 s cap would expire mid-hold.
    waitForButtonRelease(WAKE_HOLD_MAX_MS);
    mpdSetStandby(true);                        // freeze the network task
    WiFi.mode(WIFI_OFF);
    delay(250);                                 // let the Wi-Fi stack wind down

#if STANDBY_USE_DEEP
    s_rtcArm   = true;
    s_rtcPrev  = encStatePins();
    s_rtcSteps = 0;
    esp_sleep_enable_timer_wakeup((uint64_t)STANDBY_POLL_MS * 1000);
    // Say how long to hold, because the answer is the poll interval and it
    // is no longer the ~1 s it used to be.  A user who does not know this
    // will tap the knob, see nothing happen, and assume it is broken.
    Serial.printf("[standby] deep sleep, scanning knob/wheel every %u ms - "
                  "hold the knob or turn the wheel for up to %u s to wake\n",
                  (unsigned)STANDBY_POLL_MS,
                  (unsigned)((STANDBY_POLL_MS + 999) / 1000));
    esp_deep_sleep_start();                     // chip resets, setup() re-runs
    Serial.println("[standby] ERROR: deep sleep did not start!");
    esp_restart();                              // should never be reached
#else
    enableStandbyWakeup();
    esp_light_sleep_start();                    // blocks until a wakeup

    // ---- woke up in place (button or wheel) ----
    WiFi.mode(WIFI_STA);                        // the task reconnects on its own
    mpdSetStandby(false);
    hwDisplayWake();
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

    hwBegin();

    Serial.begin(115200);
    Serial.printf("%s MPD client - connecting to %s:%d\n", HW_BOARD_NAME,
                  MPD_HOST, MPD_PORT);

    gShared.mux = xSemaphoreCreateMutex();
    gCmdQueue   = xQueueCreate(12, sizeof(MpdCommand));

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
    HW.update();

    // ---- long knob press = standby -------------------------------------
#if STANDBY_PRESS_MS > 0
    static bool s_standbyArmed = false;
    if (HW.BtnA.isPressed()) {
        if (!s_standbyArmed && HW.BtnA.pressedFor(STANDBY_PRESS_MS)) {
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
    long e = HW.Encoder.read();
    long d = e - s_lastEnc;
    if (d != 0) {
        s_lastEnc = e;
        uiEncoder((int)d);
    }

    // ---- knob push ----------------------------------------------------
    if (HW.BtnA.wasClicked()) uiButtonClick();
    if (HW.BtnA.wasHold()) uiButtonHold();
    uiButtonDecide();  // single/double-click decision (browser)
    uiTouchTick();     // prev/next touch arrows in now view

    // ---- status LEDs ---------------------------------------------------
    // The ring shows the device state: a single chasing LED while playing,
    // a slow yellow breath while paused, dark when stopped, and a red pulse
    // when there is no network link.  While playing, the chase speed
    // follows the volume - louder spins faster.  Read under the shared
    // mutex because the MPD task writes these from its own task, but only
    // act when something actually changed: the animation task runs on its
    // own and does not need re-arming every pass.
    {
        bool playing, paused, wifi;
        int  volume;
        xSemaphoreTake(gShared.mux, portMAX_DELAY);
        playing = gShared.status.playing;
        paused  = gShared.status.paused;
        wifi    = gShared.wifi;
        volume  = gShared.status.volume;
        xSemaphoreGive(gShared.mux);
        static bool wasPlaying = false, wasPaused = false, wasWifi = false;
        static int  wasVolume  = -2;            // -2: never pushed
        if (playing != wasPlaying || paused != wasPaused || wifi != wasWifi ||
            volume != wasVolume) {
            wasPlaying = playing;
            wasPaused  = paused;
            wasWifi    = wifi;
            wasVolume  = volume;
            hwLedSetState(playing, paused, wifi, volume);
        }
    }

    uiTick();
    delay(4);
}