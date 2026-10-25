#include "hw.h"

#ifndef VOL_DEBUG
#define VOL_DEBUG 0
#endif

#if defined(DEVICE_CROWPANEL)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

// The single board object the firmware talks to.  On the M5Dial build the
// board object lives in the M5Dial library, so nothing to define here.
#if defined(DEVICE_CROWPANEL)

CrowPanel HW;

// ---- CrowPanel knob encoder -----------------------------------------
// One decoder shared by every translation unit that includes hw.h, hence
// the out-of-line static members.  See the KnobEncoder comment in
// include/board_crowpanel.h for why the M5Dial library's own ENCODER
// class cannot be used on GPIO 45 / 42.
// ---- CrowPanel LED ring ------------------------------------------------
// The strip handle and the animation task live here rather than in the
// header because they are process-wide singletons; only one encoder and
// one strip exist, and three translation units include hw.h.
bool         LedStrip::s_ready    = false;
rmt_channel_t LedStrip::s_channel = RMT_CHANNEL_0;
rmt_item32_t* LedStrip::s_items    = nullptr;
uint8_t*     LedStrip::s_pixels   = nullptr;
TaskHandle_t LedStrip::s_animTask = nullptr;
volatile LedStrip::Mode LedStrip::s_mode = LedStrip::MODE_OFF;
volatile uint8_t        LedStrip::s_volume = LedStrip::PLAY_VOL_DEFAULT;
volatile uint8_t        LedStrip::s_torch = 0;

volatile uint32_t* KnobEncoder::s_regA = nullptr;
uint32_t           KnobEncoder::s_maskA = 0;
volatile uint32_t* KnobEncoder::s_regB = nullptr;
uint32_t           KnobEncoder::s_maskB = 0;
volatile int32_t   KnobEncoder::s_position = 0;
volatile uint8_t   KnobEncoder::s_prev = 0;
volatile uint8_t   KnobEncoder::s_rest = 0;
volatile int8_t    KnobEncoder::s_accum = 0;
volatile int8_t    KnobEncoder::s_lastDir = 0;
uint8_t            KnobEncoder::s_cntA = 0;
uint8_t            KnobEncoder::s_cntB = 0;
volatile uint32_t  KnobEncoder::s_edges = 0;
volatile uint32_t  KnobEncoder::s_samples = 0;
volatile uint32_t  KnobEncoder::s_counted = 0;
volatile uint32_t  KnobEncoder::s_bounce = 0;
volatile uint32_t  KnobEncoder::s_threshEmits = 0;
volatile uint32_t  KnobEncoder::s_restEmits = 0;


// The encoder task samples the knob every 1 ms and does the debouncing and
// quadrature decoding in poll().  It stays a task rather than a print in
// loop() so the sample rate is unaffected by what the UI is doing and
// CROWPANEL_KNOB_DEBUG reports at a steady cadence.
static void encTask(void* arg) {
    (void)arg;
#if VOL_DEBUG
    int32_t lastPos = 0;
#endif
    for (;;) {
        KnobEncoder::poll();
#if VOL_DEBUG
        const int32_t p = HW.Encoder.read();
        if (p != lastPos) {
            lastPos = p;
            KnobEncoder::stats_t st;
            HW.Encoder.getStats(&st);
            Serial.printf("[enc] t=%lu pos=%ld cnt=%lu th=%lu rst=%lu "
                          "edges=%lu b=%lu acc=%d\n",
                          (unsigned long)millis(), (long)p,
                          (unsigned long)st.counted,
                          (unsigned long)st.threshEmits,
                          (unsigned long)st.restEmits,
                          (unsigned long)st.edges,
                          (unsigned long)st.bounce, (int)st.accum);
        }
#endif
        vTaskDelay(1);
    }
}

// One sample, called every millisecond from encTask.  The pin pair is read
// straight from the port registers, each channel is debounced on its own,
// and only a change of the *stable* pair is handed to the quadrature
// accumulator.  Contact ring therefore never reaches the accumulator, which
// is what stops a slow turn from random-walking into a phantom detent.
void KnobEncoder::poll() {
    ++s_samples;

    const uint8_t rawA = (uint8_t)((*s_regA & s_maskA) ? 1 : 0);
    const uint8_t rawB = (uint8_t)((*s_regB & s_maskB) ? 1 : 0);

    uint8_t stableA = (uint8_t)((s_prev >> 1) & 1);
    uint8_t stableB = (uint8_t)(s_prev & 1);

    // Each channel has to hold its new level for the whole debounce window
    // before the stable state follows it.  A return to the stable level
    // restarts the window, so a ringing contact just keeps resetting.
    if (rawA == stableA) {
        s_cntA = 0;
    } else if (++s_cntA >= CROWPANEL_KNOB_DEBOUNCE_SAMPLES) {
        stableA = rawA;
        s_cntA = 0;
    }
    if (rawB == stableB) {
        s_cntB = 0;
    } else if (++s_cntB >= CROWPANEL_KNOB_DEBOUNCE_SAMPLES) {
        stableB = rawB;
        s_cntB = 0;
    }

    const uint8_t now  = (uint8_t)((stableA << 1) | stableB);
    const uint8_t prev = s_prev;
    if (now == prev) return;      // no stable change: nothing to count

    // Apply one accepted quadrature step: update the reference state, the
    // accumulator, and resynchronise when the step lands on the rest state.
    auto apply = [](uint8_t p, uint8_t n, int8_t delta) {
        s_prev = n;
        ++s_edges;

        // Real rotation walks right round the quadrature square, so four
        // net transitions are one detent.
        int8_t acc = (int8_t)(s_accum + delta);
        if (acc >= 4) {
            acc = (int8_t)(acc - 4);
            ++s_position;
            ++s_counted;
            ++s_threshEmits;
        } else if (acc <= -4) {
            acc = (int8_t)(acc + 4);
            --s_position;
            ++s_counted;
            ++s_threshEmits;
        }
        s_accum = acc;

        // Every full quadrature cycle passes the resting state exactly
        // once, so arriving there means the knob is on a detent again.
        // Any remainder is error from a missed or spurious edge; pay it
        // out here and start the next detent clean.  The |acc| >= 3 gate
        // only accepts a detent one edge short, never a single bounce step.
        if (n == s_rest && p != s_rest) {
            if (s_accum >= 3) {
                ++s_position;
                ++s_counted;
                ++s_restEmits;
            } else if (s_accum <= -3) {
                --s_position;
                ++s_counted;
                ++s_restEmits;
            }
            s_accum = 0;
        }
    };

    const uint8_t changed = (uint8_t)(prev ^ now);
    if (changed == 3) {
        // Both channels reached a new stable level in the same poll: that
        // is two quadrature steps.  Recover them instead of dropping the
        // detent.  Which way round is ambiguous from the endpoints alone,
        // so trust the direction the knob was already moving.
        if (s_lastDir == 0) {
            // Nothing to trust yet (first movement after boot), so drop the
            // ambiguous pair but adopt the new state, otherwise this poll
            // would see the same diagonal again every millisecond.
            ++s_bounce;
            s_prev = now;
            return;
        }
        // Forward step: A' = !B, B' = A.  Reverse step: A' = B, B' = !A.
        const uint8_t mid = (s_lastDir > 0)
            ? (uint8_t)((((prev & 1) ? 0 : 2)) | ((prev & 2) ? 1 : 0))
            : (uint8_t)(((prev & 1) ? 2 : 0) | ((prev & 2) ? 0 : 1));
        apply(prev, mid, s_lastDir);
        apply(mid, now, s_lastDir);
        return;
    }

    int8_t delta;
    if (changed == 2) {                       // channel A moved
        delta = (now & 2) ? 1 : -1;           // rising A ...
        if (prev & 1) delta = (int8_t)-delta; // ... only forward when B low
    } else {                                  // channel B moved
        delta = (now & 1) ? 1 : -1;
        if (!(prev & 2)) delta = (int8_t)-delta;
    }
#if !CROWPANEL_KNOB_INVERT
    delta = (int8_t)-delta;
#endif
    s_lastDir = (delta > 0) ? 1 : -1;
    apply(prev, now, delta);
}

void KnobEncoder::begin(uint8_t pinA, uint8_t pinB) {
    pinMode(pinA, INPUT_PULLUP);
    pinMode(pinB, INPUT_PULLUP);

    s_regA  = portInputRegister(digitalPinToPort(pinA));
    s_maskA = digitalPinToBitMask(pinA);
    s_regB  = portInputRegister(digitalPinToPort(pinB));
    s_maskB = digitalPinToBitMask(pinB);

    // Start from the real pin state, otherwise the first edge can look like
    // a transition from the reset value and be miscounted.
    s_prev     = (uint8_t)(((*s_regA & s_maskA) ? 2 : 0) |
                           ((*s_regB & s_maskB) ? 1 : 0));
    s_rest     = s_prev;
    s_accum    = 0;
    s_lastDir  = 0;
    s_cntA     = 0;
    s_cntB     = 0;
    s_position = 0;
    s_samples = s_counted = s_bounce = s_edges = 0;

    // Started only now, once the port registers are known, because the task
    // reads them directly.  A stack of 3 KB is ample: it is a poll and a
    // delay, and keeping it off the PSRAM path keeps the wake latency down.
    xTaskCreate(encTask, "enc", 3072, nullptr, 5, nullptr);
}

#endif  // DEVICE_CROWPANEL
