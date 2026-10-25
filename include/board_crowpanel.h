#pragma once

// =====================================================================
//  Hardware backend - Elecrow CrowPanel 1.28" (SKU DHE38128D)
//
//  Same contract as the M5Dial backend: a global board object `HW` that
//  offers display, touch, knob button and encoder, plus the geometry and
//  pin macros main.cpp needs for the standby engines.  See hw.h.
//
//  Board facts (ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM):
//    GC9A01 240x240 round IPS  SPI  SCLK 10 / MOSI 11 / DC 3 / CS 9 /
//                                    RST 14, backlight 46
//    CST816D capacitive touch I2C  SDA 6 / SCL 7 / INT 5 / RST 13
//    rotary knob  A 45 / B 42, push switch 41 (active low)
//    panel rails  1 (LCD_3V3) and 2 (LEDA_3V3), both must be HIGH
// =====================================================================

#include <Arduino.h>

#include <M5GFX.h>
#include <M5Unified.h>          // m5::Button_Class / m5::Touch_Class only

// M5GFX.h only pulls in the generic device classes; the concrete drivers
// used below have to be requested by hand.
#include <lgfx/v1/panel/Panel_GC9A01.hpp>
#include <lgfx/v1/touch/Touch_CSTxxx.hpp>

// RMT for the ws2812 strip.  The ESP-IDF bundled in this core is 4.4, whose
// ws2812_led component is the old single-pixel API and its led_strip
// component is not prebuilt for Arduino, so the strip is driven through
// RMT directly - which is what led_strip does internally anyway.
//
// RMT rather than bit-banging because a bit-banged frame that is preempted
// part way through corrupts every pixel after the interruption, and this
// firmware has a lot to preempt it: Wi-Fi, MPD, KNX and the UI all run
// tasks.  RMT builds the whole frame in a DMA-capable buffer and the
// hardware clocks it out with no CPU involvement.
#include <driver/rmt.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

#define HW_BOARD_NAME "CrowPanel 1.28"

// ---- board pin map (keep in sync with variants/crowpanel_128) ---------
#define CROWPANEL_PIN_LCD_PWR    1    // LCD_3V3 rail enable
#define CROWPANEL_PIN_LED_PWR    2    // LEDA_3V3 rail enable
#define CROWPANEL_PIN_LCD_DC     3
#define CROWPANEL_PIN_TP_INT     5
#define CROWPANEL_PIN_TP_SDA     6
#define CROWPANEL_PIN_TP_SCL     7
#define CROWPANEL_PIN_LCD_CS     9
#define CROWPANEL_PIN_LCD_SCLK  10
#define CROWPANEL_PIN_LCD_MOSI  11
#define CROWPANEL_PIN_TP_RST    13
#define CROWPANEL_PIN_LCD_RST   14
#define CROWPANEL_PIN_KNOB_SW   41    // push switch, active low
#define CROWPANEL_PIN_KNOB_B    42
#define CROWPANEL_PIN_KNOB_A    45
#define CROWPANEL_PIN_LCD_BL    46
#define CROWPANEL_PIN_LED_RGB   48    // WS2812 strip data

// The five addressable pixels ring the panel, and the vendor demo drives
// them as a 5-pixel NEO_GRB strip.
#define CROWPANEL_LED_COUNT      5

// ---- input pins scanned by the standby engines ------------------------
#define HW_ENC_A      CROWPANEL_PIN_KNOB_A
#define HW_ENC_B      CROWPANEL_PIN_KNOB_B
#define HW_BTN_PIN    CROWPANEL_PIN_KNOB_SW
#define HW_BTN_LEVEL  0        // the knob push pulls to ground

// ---- panel + logical UI geometry ---------------------------------------
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

#define HW_HAS_ENCODER 1
#define HW_HAS_TOUCH   1

// Backlight level used at boot (0-255).  127 is M5GFX's own default and
// matches how bright the M5Dial build looks; drop it if the panel runs hot.
#define HW_BACKLIGHT  127

// ---------------------------------------------------------------------
//  Display: a plain M5GFX with a hand wired GC9A01 + CST816D.  M5Unified
//  has no board entry for the CrowPanel, so M5GFX's autodetect() is
//  bypassed and bus / panel / touch / backlight are wired up by hand.
//  The result is still an M5GFX, which is what M5Canvas (the 240x240
//  PSRAM sprite the UI draws into) wants as its parent.
// ---------------------------------------------------------------------
class CrowPanelGfx : public M5GFX {
protected:
    bool init_impl(bool use_reset, bool use_clear) override {
        // ---- power rails: both must be high before the panel comes up
        pinMode(CROWPANEL_PIN_LCD_PWR, OUTPUT);
        pinMode(CROWPANEL_PIN_LED_PWR, OUTPUT);
        digitalWrite(CROWPANEL_PIN_LCD_PWR, HIGH);
        digitalWrite(CROWPANEL_PIN_LED_PWR, HIGH);

        // ---- SPI bus (3 wire, the panel has no MISO)
        {
            auto cfg = bus_.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 80000000;
            cfg.freq_read   = 20000000;
            cfg.spi_3wire   = true;
            cfg.use_lock    = true;
            cfg.pin_sclk    = CROWPANEL_PIN_LCD_SCLK;
            cfg.pin_mosi    = CROWPANEL_PIN_LCD_MOSI;
            cfg.pin_miso    = -1;
            cfg.pin_dc      = CROWPANEL_PIN_LCD_DC;
            bus_.config(cfg);
        }

        // ---- GC9A01 round panel
        {
            auto cfg = panel_.config();
            cfg.pin_cs          = CROWPANEL_PIN_LCD_CS;
            cfg.pin_rst         = CROWPANEL_PIN_LCD_RST;
            cfg.pin_busy        = -1;
            cfg.memory_width    = 240;
            cfg.memory_height   = 240;
            cfg.panel_width     = 240;
            cfg.panel_height    = 240;
            cfg.offset_x        = 0;
            cfg.offset_y        = 0;
            cfg.offset_rotation = 0;
            cfg.readable        = false;
            cfg.invert          = true;   // IPS
            cfg.bus_shared      = false;
            panel_.config(cfg);
        }
        panel_.setBus(&bus_);

        // ---- CST816D touch (register map of the CST816S driver, 0x15)
        {
            auto cfg = touch_.config();
            cfg.i2c_port        = 1;
            cfg.i2c_addr        = 0x15;
            cfg.pin_sda         = CROWPANEL_PIN_TP_SDA;
            cfg.pin_scl         = CROWPANEL_PIN_TP_SCL;
            cfg.pin_int         = CROWPANEL_PIN_TP_INT;
            cfg.pin_rst         = CROWPANEL_PIN_TP_RST;
            cfg.freq            = 400000;
            cfg.x_min           = 0;
            cfg.x_max           = 239;
            cfg.y_min           = 0;
            cfg.y_max           = 239;
            cfg.offset_rotation = 0;
            cfg.bus_shared      = false;
            touch_.config(cfg);
        }
        panel_.touch(&touch_);

        // ---- PWM backlight on GPIO46
        {
            auto cfg = light_.config();
            cfg.pin_bl       = CROWPANEL_PIN_LCD_BL;
            cfg.pwm_channel = 7;
            cfg.freq        = 44100;
            cfg.invert      = false;
            light_.config(cfg);
        }
        panel_.setLight(&light_);

        setPanel(&panel_);
        return LGFX_Device::init_impl(use_reset, use_clear);
    }

    lgfx::Bus_SPI       bus_;
    lgfx::Panel_GC9A01  panel_;
    lgfx::Touch_CST816S touch_;
    lgfx::Light_PWM     light_;
};

// ---------------------------------------------------------------------
//  The five addressable LEDs ringing the panel.
//
//  Driven through RMT rather than bit-banged, because a bit-banged frame
//  that is preempted part way through corrupts every pixel after the
//  interruption and this firmware has plenty to preempt it: Wi-Fi, MPD,
//  KNX and the UI all run tasks.  RMT builds the frame in a DMA-capable
//  buffer and the hardware clocks it out with no CPU involvement, so the
//  animation stays smooth while the network stack is busy.
//
//  The pixels sit behind the LEDA_3V3 rail (pin 2), which init_impl()
//  already enables, so begin() only configures the strip itself.
class LedStrip {
public:
    // WS2812 bit timings at the RMT source clock.  These are the standard
    // datasheet values (T0H 0.4us, T0L 0.85us, T1H 0.8us, T1L 0.45us)
    // converted to clock ticks; the source is the APB clock divided down by
    // a factor of 2, i.e. 40 MHz here, so a tick is 25 ns.  The tolerances
    // on a WS2812 are generous, hence the round numbers.
    static constexpr uint32_t RMT_CLK_DIV      = 2;
    static constexpr uint32_t RMT_TICKS_PER_US = 40;   // 40 MHz source clock
    static constexpr uint16_t T0H = 0.4f * RMT_TICKS_PER_US;   // 16
    static constexpr uint16_t T0L = 0.9f * RMT_TICKS_PER_US;   // 36
    static constexpr uint16_t T1H = 0.8f * RMT_TICKS_PER_US;   // 32
    static constexpr uint16_t T1L = 0.4f * RMT_TICKS_PER_US;   // 16
    // >50us of low level latches the strip.  Needs its own item, since the
    // 15us item limit applies to each item.
    static constexpr uint16_t LATCH = 3000 / RMT_TICKS_PER_US;  // 75us

    // One item per bit (a 32-bit item holds a high time and a low time, so
    // a whole bit fits in one) plus the latch item.
    static constexpr int ITEM_COUNT = CROWPANEL_LED_COUNT * 24 + 1;

    void begin() {
        if (s_ready) return;

        // This IDF predates RMT_DEFAULT_CONFIG(), so the struct is filled
        // in by hand.  mem_block_num 1 is what a TX-only channel wants.
        rmt_config_t rc = {};
        rc.rmt_mode      = RMT_MODE_TX;
        rc.channel       = RMT_CHANNEL_0;
        rc.gpio_num      = (gpio_num_t)CROWPANEL_PIN_LED_RGB;
        rc.clk_div       = RMT_CLK_DIV;
        rc.mem_block_num = 1;
        if (rmt_config(&rc) != ESP_OK) {
            Serial.println("[led] rmt_config failed");
            return;
        }
        if (rmt_driver_install(rc.channel, 0, 0) != ESP_OK) {
            Serial.println("[led] rmt_driver_install failed");
            return;
        }
        s_channel = rc.channel;

        // Hold the line actively low between frames.  A WS2812 latches on
        // >50 us of low, and that is what tells it the frame is finished,
        // so the idle level cannot be left to chance.
        rmt_set_idle_level(s_channel, true, RMT_IDLE_LEVEL_LOW);

        // The RMT driver reads this buffer from its own interrupt context,
        // so it has to be in internal DMA-capable RAM.  A plain malloc() may
        // well hand back PSRAM on this board, which an ISR cannot touch, and
        // the symptom when that happens is a corrupted bitstream - the
        // pixels come out the wrong colour rather than failing outright.
        s_items = (rmt_item32_t*)heap_caps_malloc(ITEM_COUNT * sizeof(rmt_item32_t),
                                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_items) {
            Serial.println("[led] no memory for RMT items");
            return;
        }
        s_pixels = (uint8_t*)calloc(CROWPANEL_LED_COUNT, 3);
        if (!s_pixels) {
            free(s_items);
            s_items = nullptr;
            Serial.println("[led] no memory for pixel buffer");
            return;
        }
        s_ready = true;
    }

    void set(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
        if (!s_ready || index >= CROWPANEL_LED_COUNT) return;
        s_pixels[index * 3 + 0] = r;
        s_pixels[index * 3 + 1] = g;
        s_pixels[index * 3 + 2] = b;
    }

    // Latch the buffer out to the strip.  Blocks for the length of one
    // frame, about 750 us for 5 pixels, which is short enough not to matter
    // and is what keeps the animation in step.
    void show() {
        if (!s_ready) return;
        int n = 0;
        for (uint8_t p = 0; p < CROWPANEL_LED_COUNT; ++p) {
            // The wire order is G, R, B: index 0 goes out first.  This is
            // the classic "why is it the wrong colour" bug - a strip wired
            // GRB that is sent RGB shows the channels permuted.
            const uint8_t grb[3] = { s_pixels[p * 3 + 1],   // green
                                     s_pixels[p * 3 + 0],   // red
                                     s_pixels[p * 3 + 2] }; // blue
            // Walk the bytes in wire order and each byte MSB first.  This
            // has to be byte 0 then bit 7 downwards: iterating a single
            // 23..0 bit counter instead reads index 2 first, so the three
            // bytes go out reversed and a green pixel arrives as blue,
            // because the strip treats the first byte it receives as green.
            for (uint8_t byte = 0; byte < 3; ++byte) {
              for (int8_t bit = 7; bit >= 0; --bit) {
                const bool one = (grb[byte] >> bit) & 1;
                s_items[n].level0 = 1;
                s_items[n].duration0 = one ? T1H : T0H;
                s_items[n].level1 = 0;
                s_items[n].duration1 = one ? T1L : T0L;
                // Both bit timings are well inside the 15us per level that
                // a 32-bit item can hold, so a whole bit - high time then
                // low time - is one item and the next bit starts in the
                // following item.
                ++n;
              }
            }
        }
        s_items[n].level0 = 0;
        s_items[n].duration0 = LATCH;
        s_items[n].level1 = 0;
        s_items[n].duration1 = LATCH;
        ++n;
        rmt_write_items(s_channel, s_items, n, true);
    }

    void clear() {
        if (!s_ready) return;
        memset(s_pixels, 0, CROWPANEL_LED_COUNT * 3);
        show();
    }

    // Start the animation task.  It plays the blue boot greeting, then
    // settles into whatever transport state it is told about.
    //
    // "Turn" is the point of the greeting: the five pixels sit in a ring
    // around the rotary knob, so a bright pixel travelling around them
    // reads as that knob turning, which is the gesture the user is about
    // to make.  It is deliberately the same three-turn chase the shutdown
    // uses, just in blue, so the two ends of a session look alike.
    //
    // Afterwards the ring follows the transport state: one green pixel
    // circling while music plays, a yellow breath while paused, nothing
    // when stopped, red when there is no network.
    void start() {
        if (!s_ready || s_animTask) return;      // one animation task
        if (xTaskCreate(animTask, "led", 3072, this, 4, &s_animTask) != pdPASS) {
            s_animTask = nullptr;
            Serial.println("[led] startup animation task failed");
        }
    }

    // Transport state the animation reacts to.  Plain integers rather than
    // an enum member so the value can be stored in a volatile byte and
    // read by the animation task without a lock.
    enum Mode : uint8_t { MODE_OFF = 0, MODE_PLAY, MODE_PAUSE, MODE_NOWIFI };

    // Safe to call from the UI task at any time, including while the
    // animation is running.  A single byte store, so a torn read is not
    // possible and no mutex is needed.
    void setMode(Mode m) { s_mode = m; }

    // Current volume on MPD's 0..100 scale; anything outside that is
    // treated as unknown and ignored, so a server that reports -1 (no
    // mixer) leaves the animation on its default speed instead of pinning
    // it to the slowest or fastest setting.
    void setVolume(int vol) {
        if (vol < 0 || vol > 100) return;
        s_volume = (uint8_t)vol;
    }

    // Torch: hold the whole ring at full white so the panel can be used as
    // a light.  Toggled from the menu; the animation task gives it priority
    // over the transport state, and clearing it lets the normal chase or
    // breath come straight back.
    void toggleTorch() { s_torch = s_torch ? 0 : 1; }
    bool torchOn() const { return s_torch != 0; }

    // False if RMT or the buffers failed to come up, in which case there is
    // no strip to drive and the animation task never started.
    bool ready() const { return s_ready; }

    // Blocking goodbye: a red dot circles SHUTDOWN_TURNS times, then the
    // strip goes dark.  Any active torch is cancelled first, otherwise the
    // animation task would keep repainting the white torch frame over the
    // goodbye.
    //
    // This runs on the caller's task, not the animation task, and it has
    // to: the LED rail (pin 2) is cut by hwDisplaySleep() a moment later
    // and once that happens the pixels are dead regardless of what the
    // animation task is doing.  It also blocks on purpose - this is the
    // last thing the device does before it sleeps, so there is no UI to
    // keep responsive and no point yielding to tasks that are about to be
    // frozen anyway.
    //
    // Returns the time it took, so the caller can see whether the sequence
    // really had room to run.
    uint32_t shutdownTurn() {
        if (!s_ready) return 0;

        // Stop the background animation first, or it would immediately
        // overwrite these frames with whatever transport state is current.
        s_mode  = MODE_OFF;
        s_torch = 0;
        clear();
        vTaskDelay(pdMS_TO_TICKS(20));

        const uint32_t tookMs = spin(SPIN_RED_R, SPIN_RED_G, SPIN_RED_B,
                                     SPIN_TURNS, SPIN_PERIOD_MS);
        clear();                   // off, as asked
        return tookMs;
    }

private:
    static void animTask(void* arg) {
        static_cast<LedStrip*>(arg)->runLoop();
        s_animTask = nullptr;            // allow another animation later
        vTaskDelete(nullptr);
    }

    // The animation task lives as long as the firmware does.  It used to
    // run for a fixed duration and delete itself, which was right for a
    // one-shot boot animation but cannot express "keep circling while
    // playing", so it now idles at whatever mode it is told to be in.
    void runLoop() {
        uint8_t  head       = 0;
        bool     bootDone   = false;
        uint32_t startMs    = millis();
        // Last mode actually drawn, so the stopped case can go quiet
        // instead of re-sending a black frame forever.
        Mode     drawn      = MODE_OFF;
        // White is a static frame, so it is only sent when the torch is
        // first switched on; the task keeps waking to see it switched off.
        bool     torchDrawn = false;

        for (;;) {
            // The torch outranks the transport animation.  Toggling it off
            // clears the ring and forces the current state to be redrawn on
            // the next pass, including the stopped (dark) state.
            if (s_torch) {
                if (!torchDrawn) {
                    for (uint8_t i = 0; i < CROWPANEL_LED_COUNT; ++i)
                        set(i, 255, 255, 255);
                    show();
                    torchDrawn = true;
                }
                vTaskDelay(pdMS_TO_TICKS(TORCH_FRAME_MS));
                continue;
            }
            if (torchDrawn) {
                torchDrawn = false;
                clear();
                drawn = MODE_OFF;
            }

            // The boot greeting owns the strip for its first few seconds,
            // so the LEDs say something at startup regardless of what MPD
            // happens to be doing.  It is the same three-turn chase as the
            // shutdown goodbye, in blue, so the two ends of the session
            // read as the same gesture in different colours.  It runs on
            // this task, which is why blocking here is fine: nothing else
            // is waiting on the LED task, and the caller is not waiting on
            // it either - the encoder task is separate and still runs.
            if (!bootDone) {
                drawn = MODE_PLAY;
                spin(SPIN_BOOT_R, SPIN_BOOT_G, SPIN_BOOT_B,
                     SPIN_BOOT_TURNS, SPIN_PERIOD_MS);
                clear();               // dark before handing over
                bootDone = true;
                continue;
            }

            switch (s_mode) {
                // Offline outranks everything else: a ring showing a green
                // chase or a yellow breath while the panel is off the
                // network is reporting a state the device is not in, so
                // this is checked first.
                case MODE_NOWIFI:
                    drawn = MODE_NOWIFI;
                    renderOffline(millis());
                    vTaskDelay(pdMS_TO_TICKS(OFFLINE_FRAME_MS));
                    break;

                case MODE_PLAY:  drawn = MODE_PLAY;
                                 renderPlay(head, millis() - startMs);
                                 head = (uint8_t)((head + 1) % CROWPANEL_LED_COUNT);
                                 vTaskDelay(pdMS_TO_TICKS(playStepMs()));
                                 break;

                case MODE_PAUSE: drawn = MODE_PAUSE;
                                 renderPause(millis());
                                 vTaskDelay(pdMS_TO_TICKS(PAUSE_FRAME_MS));
                                 break;

                default:
                    // Stopped: latch the strip dark once, then stop
                    // transmitting.  Sending the same all-zero frame every
                    // poll would keep waking the RMT hardware and the task
                    // for a display that is already off.
                    if (drawn != MODE_OFF) { clear(); drawn = MODE_OFF; }
                    vTaskDelay(pdMS_TO_TICKS(OFF_POLL_MS));
            }
        }
    }

    // A revolution is CROWPANEL_LED_COUNT head advances, so the step is
    // the period divided by the pixel count and nothing else.  An earlier
    // version divided by 40 "for smoothness" while the head still moved a
    // whole pixel per step, which made a revolution take 5 steps - 375 ms
    // rather than the 3 s the constant claimed, and the comet went round
    // eight times too fast.
    //
    // How fast the playing-state LED travels, as a function of volume.
    //
    // Louder spins faster: the ring is a level meter you can read from
    // across the room without walking over to the panel, so the mapping has
    // to be monotonic in the obvious direction.  A stationary dot and a
    // fast blur are the two ends, and the useful reading is somewhere in
    // between, hence a wide range rather than a gentle one.
    //
    // The floor matters as much as the span: at full volume the step still
    // has to be long enough for the dot to be seen to land, or the motion
    // turns into a strobe and the speed stops being readable at all.
    static constexpr uint32_t PLAY_PERIOD_SLOW_MS = 2000;  // volume 0
    static constexpr uint32_t PLAY_PERIOD_FAST_MS = 500;   // volume 100
    // The volume the animation starts at before MPD has reported one.
    static constexpr uint8_t  PLAY_VOL_DEFAULT   = 50;

    // Linear interpolation between the two periods, then divided by the
    // pixel count to get one step.  Kept as a function because the period
    // changes as the knob is turned, and it has to be re-read every step
    // rather than latched once.
    uint32_t playStepMs() const {
        const uint32_t span = PLAY_PERIOD_SLOW_MS - PLAY_PERIOD_FAST_MS;
        const uint32_t period = PLAY_PERIOD_SLOW_MS -
            (span * (uint32_t)s_volume) / 100u;
        const uint32_t step = period / CROWPANEL_LED_COUNT;
        // Never round a step down to zero: a 0 ms delay would spin the dot
        // as fast as the task can run, which is both a strobe and a waste
        // of CPU on a 5 pixel ring.
        return step ? step : 1;
    }

    // A raised cosine over the period, so the ring breathes between off and
    // full.  Every pixel gets the same level here, which is the point: the
    // paused state is a uniform glow, so there is no bright head to follow
    // and a slow rate is not masked the way it was in the playing pattern.
    static constexpr uint32_t PAUSE_PERIOD_MS = 2400;
    // A fade needs many frames, or it steps.  20 ms is 50 Hz, smooth
    // without spending the frame budget on a 5 pixel ring.
    static constexpr uint32_t PAUSE_FRAME_MS  = 20;
    // Nothing to do when stopped, so wake rarely.
    static constexpr uint32_t OFF_POLL_MS     = 250;
    // The torch frame is static, so it only has to be transmitted when the
    // state changes; the task still wakes at this rate to notice a toggle.
    static constexpr uint32_t TORCH_FRAME_MS  = 200;
    // The one-pixel chase that opens and closes the session.  Blue on the
    // way up, red on the way down, so which end of the lifecycle you are
    // looking at is readable at a glance from the colour alone.
    // The two chases are separate rather than shared because they want
    // different lengths.  The greeting runs three times as long as the
    // goodbye: boot is a moment the user is present for and waits on, so a
    // longer, more obvious flourish is welcome, whereas the goodbye is on
    // the way to sleep and should not outstay its welcome.  The rotation
    // speed is the same for both - what changed is the number of turns, so
    // the greeting is the same motion simply carried on for longer rather
    // than a sluggish crawl.
    static constexpr uint8_t  SPIN_BOOT_TURNS = 9;   // 3 x the goodbye
    static constexpr uint8_t  SPIN_TURNS     = 3;
    static constexpr uint32_t SPIN_PERIOD_MS = 500;
    // Blue, and red, as a full-scale RGB triple.  A WS2812 has no blue or
    // red "channel" in the logical sense; these are the three die elements
    // driven independently, and green is left at zero for both so the hue
    // cannot be contaminated by the playing pattern's green.
    static constexpr uint8_t SPIN_BOOT_R = 0,   SPIN_BOOT_G = 0,   SPIN_BOOT_B = 255;
    static constexpr uint8_t SPIN_RED_R  = 255, SPIN_RED_G  = 0,   SPIN_RED_B  = 0;

    // Offline pulse: slow, and obviously a fault rather than a state.
    static constexpr uint32_t OFFLINE_PERIOD_MS = 1600;
    static constexpr uint32_t OFFLINE_FRAME_MS  = 20;

    // The one-pixel chase, used for both the boot greeting and the
    // shutdown goodbye - same motion, only the colour differs.  Blocking,
    // for the reasons given on shutdownTurn().  Returns the elapsed time.
    uint32_t spin(uint8_t r, uint8_t g, uint8_t b,
                  uint8_t turns, uint32_t periodMs) {
        const uint32_t startMs = millis();
        const uint32_t stepMs  = periodMs / CROWPANEL_LED_COUNT;
        const uint32_t totalMs = periodMs * turns;
        uint8_t head = 0;

        while (millis() - startMs < totalMs) {
            const uint32_t elapsed = millis() - startMs;
            // Shimmer scales every channel together, so the hue stays put
            // through the fade instead of drifting as one channel falls
            // away faster than the others.
            const float    gain   = 0.65f + 0.35f * sinf(
                (float)elapsed / 1000.0f * 2.0f * PI * 5.0f);
            const uint8_t  cr = (uint8_t)(r * gain);
            const uint8_t  cg = (uint8_t)(g * gain);
            const uint8_t  cb = (uint8_t)(b * gain);
            // Exactly one pixel lit; the rest are truly zero, not a dim
            // residue, so the ring reads as one dot on a dark ring.
            for (uint8_t i = 0; i < CROWPANEL_LED_COUNT; ++i) {
                set(i, i == head ? cr : 0,
                       i == head ? cg : 0,
                       i == head ? cb : 0);
            }
            show();
            head = (uint8_t)((head + 1) % CROWPANEL_LED_COUNT);
            vTaskDelay(pdMS_TO_TICKS(stepMs));
        }
        return millis() - startMs;
    }

    void renderPlay(uint8_t head, uint32_t elapsed) {
        // One lit pixel chasing round a dark ring.
        //
        // Earlier versions left a two-pixel tail and a faint base glow so
        // the rotation would read unambiguously.  On a symmetric ring that
        // is not needed here - the direction is given by the knob the LEDs
        // surround, not by the pattern - and the dim leftovers were just
        // visible haze that made the ring look half on.  So exactly one
        // pixel is lit and the rest are truly zero.
        //
        // The shimmer rides on that single pixel.  It has to stay a
        // different rate from the rotation: when the pulse scaled all the
        // pixels together the eye followed the overall brightness and lost
        // track of where the bright one was, which made the rotation look
        // like nothing was moving at all.
        const float shimmer = 0.65f + 0.35f * sinf(
            (float)elapsed / 1000.0f * 2.0f * PI * 5.0f);
        const uint8_t v = (uint8_t)(255.0f * shimmer);

        for (uint8_t i = 0; i < CROWPANEL_LED_COUNT; ++i) {
            set(i, 0, i == head ? v : 0, 0);      // green: green only
        }
        show();
    }

    // Offline: the whole ring pulses red.  Red is used nowhere else, so a
    // red ring means "no network" and nothing else.  Like the paused
    // breath this is a raised cosine over the whole ring, but at a
    // different rate and a full off-to-on swing rather than a dim level,
    // because it is an error indication and should be hard to mistake for
    // a paused device.
    void renderOffline(uint32_t elapsed) {
        const float phase   = (float)elapsed / (float)OFFLINE_PERIOD_MS
                               * 2.0f * PI;
        const float pulse   = 0.5f - 0.5f * cosf(phase);
        const uint8_t v     = (uint8_t)(pulse * 255.0f);
        for (uint8_t i = 0; i < CROWPANEL_LED_COUNT; ++i) set(i, v, 0, 0);
        show();
    }

    // Paused: the whole ring breathes between off and full, slowly and
    // uniformly.  No comet, because a moving highlight while paused would
    // read as playing.
    void renderPause(uint32_t elapsed) {
        const float phase   = (float)elapsed / (float)PAUSE_PERIOD_MS
                               * 2.0f * PI;
        const float breathe = 0.5f - 0.5f * cosf(phase);
        // Full at the top of the breath, fully off at the bottom.
        const uint8_t v = (uint8_t)(breathe * 255.0f);
        // Yellow, not green.  A WS2812 is a single die with an R, a G and a
        // B element, so yellow is red and green driven together with blue
        // at zero - there is no yellow channel to address.  Both get the
        // same level so the hue stays yellow at every point of the fade
        // instead of shifting through orange as one channel fell away.
        for (uint8_t i = 0; i < CROWPANEL_LED_COUNT; ++i) set(i, v, v, 0);
        show();
    }

    // There is one strip and one animation, and three translation units
    // include this header, so the state is defined once in src/hw.cpp.
    static bool         s_ready;
    static rmt_channel_t s_channel;
    static rmt_item32_t* s_items;
    static uint8_t*     s_pixels;
    static TaskHandle_t  s_animTask;
    // Written by setMode() from the UI, read by the animation task.
    static volatile Mode s_mode;
    // Written by setVolume() from the UI, read by the animation task.
    static volatile uint8_t s_volume;
    // Torch flag; 0 = normal transport animation, 1 = all white.
    static volatile uint8_t s_torch;
};

// ---------------------------------------------------------------------
//  Rotary knob encoder.
//
//  The PJRC ENCODER class that ships with the M5Dial library only knows
//  how to attach an interrupt to the fixed pin list in
//  utility/Encoder.h (GPIO 0-5, 12-18, 21-23, 25-27, 32-36, 39).  The
//  CrowPanel knob is on GPIO 45 / 42, which are not on that list, so the
//  class silently drops to sampling both pins once per read().  With the
//  UI redraw, the Wi-Fi and MPD work in the loop, that samples far too
//  slowly and the count aliases: the knob jumps around instead of
//  turning smoothly, and can even appear to run backwards.  So decode
//  the encoder here instead.
//
//  Two earlier decoders failed in opposite directions: the factory-style
//  rising-edge counter let contact ring through as phantom steps, and a
//  debounced sampling decoder that sampled from loop() rejected fast
//  turns because at speed a quadrature transition can be shorter than
//  the stable window.
//
//  This version samples both channels at 1 ms from the dedicated encoder
//  task, and debounces each channel independently: a channel's stable
//  level only follows the pin after it has held the new level for
//  CROWPANEL_KNOB_DEBOUNCE_SAMPLES polls.  Contact ring never reaches the
//  quadrature accumulator, so it cannot random-walk into a phantom detent.
//
//  Only stable-state changes are fed into the same full-quadrature
//  accumulation, rest resync and diagonal momentum recovery as before.
//  Four net transitions are one detent.
//
//  Only one encoder exists, so the state lives in static members.  poll()
//  is defined out of line in src/hw.cpp, and the three TUs that include
//  this header share one decoder.
// ---------------------------------------------------------------------

// Set to 1 if the knob counts the wrong way round.
#ifndef CROWPANEL_KNOB_INVERT
#define CROWPANEL_KNOB_INVERT 1
#endif

// Prints the encoder counters once a second.  This is what established
// that the decoder was working, and it is worth being able to turn back on
// if the knob misbehaves, but it is off for normal running: 1 Hz is
// nowhere near the print rate that flooded USB CDC and tripped the task
// watchdog earlier.
#ifndef CROWPANEL_KNOB_DEBUG
#define CROWPANEL_KNOB_DEBUG 0
#endif

// How many consecutive 1 ms polls a channel must hold a new level before
// that level becomes the stable state.  Four samples rejects the ringing
// seen on this knob (hundreds of edges per second) while still resolving a
// normal turn, where one quadrature step lasts many milliseconds.
#ifndef CROWPANEL_KNOB_DEBOUNCE_SAMPLES
#define CROWPANEL_KNOB_DEBOUNCE_SAMPLES 4
#endif

class KnobEncoder {
public:
    void begin(uint8_t pinA, uint8_t pinB);

    // s_position is written by the encoder task and read here.  It is a
    // naturally aligned 32-bit word, so a single load cannot be torn, and a
    // torn read is the only failure that matters: a one-step error in
    // position cancels out on the next read, since the caller works in
    // deltas.  So no critical section is needed, and none should be used -
    // interrupts()/interrupts() from a task would re-enable interrupts that
    // the caller may have deliberately masked.
    int32_t read() const { return s_position; }

    void reset() { s_position = 0; }

    // One 1 ms sample: debounce each channel, then feed any stable-state
    // change into the quadrature accumulator.  Called from the encoder
    // task in src/hw.cpp.
    static void poll();

    // --- diagnostics, read from loop context only
    struct stats_t {
        int32_t  position;
        uint8_t  raw;        // bit0 = A, bit1 = B, via the port registers
        uint8_t  rawDr;      // the same, via digitalRead(), as a cross-check
        uint32_t samples;    // poll() calls (heartbeat only)
        uint32_t counted;    // detents released, i.e. real steps
        uint32_t bounce;     // impossible two-bit transitions
        uint32_t edges;      // accepted stable-state transitions
        uint32_t threshEmits; // detents emitted at 4 net transitions
        uint32_t restEmits;   // extra detents emitted by rest resync
        int8_t   accum;      // accumulator remainder
    };
    void getStats(stats_t* out) {
        out->position = s_position;
        out->samples  = s_samples;
        out->counted  = s_counted;
        out->bounce   = s_bounce;
        out->edges    = s_edges;
        out->threshEmits = s_threshEmits;
        out->restEmits   = s_restEmits;
        out->accum       = s_accum;
        out->raw = (uint8_t)((*s_regA & s_maskA) ? 1 : 0) |
                   (uint8_t)((*s_regB & s_maskB) ? 2 : 0);
        // digitalRead() as a cross-check, so that a wrong port-register read
        // cannot be mistaken for a bad encoder.
        out->rawDr = (uint8_t)((KnobEncoder::level(0) ? 1 : 0) |
                               (KnobEncoder::level(1) ? 2 : 0));
    }

    // Raw level of one encoder channel, for comparing the port-register
    // read against what the pin actually reads.
    static int level(uint8_t which) {
        return digitalRead(which ? CROWPANEL_PIN_KNOB_B
                                : CROWPANEL_PIN_KNOB_A);
    }

private:
    static volatile uint32_t* s_regA;
    static uint32_t           s_maskA;
    static volatile uint32_t* s_regB;
    static uint32_t           s_maskB;

    static volatile int32_t   s_position;   // detents, signed
    static volatile uint8_t   s_prev;       // last stable state, bit1=A bit0=B
    static volatile uint8_t   s_rest;       // state the knob sits at between detents
    static volatile int8_t    s_accum;      // quadrature transitions towards a detent
    static volatile int8_t    s_lastDir;    // last accepted direction, 0 = unknown
    static uint8_t            s_cntA;       // polls raw A has differed from stable
    static uint8_t            s_cntB;       // polls raw B has differed from stable
    static volatile uint32_t  s_edges;      // accepted stable-state transitions
    static volatile uint32_t  s_samples;    // poll() heartbeat
    static volatile uint32_t  s_counted;    // detents released
    static volatile uint32_t  s_bounce;     // impossible transitions
    static volatile uint32_t  s_threshEmits; // detents emitted by the 4-step rule
    static volatile uint32_t  s_restEmits;   // detents emitted by rest resync
};

// ---------------------------------------------------------------------
//  The board object: everything the firmware touches lives here.  Mirrors
//  the API of the m5::M5_DIAL object the M5Dial build uses, so main.cpp
//  and ui.cpp are identical on both boards.
//
//  IMPORTANT: Display/Touch/BtnA/Encoder are *direct object members*, not
//  references.  ui.cpp takes `&HW.Display` in a file-scope initializer
//  (`static M5Canvas spr(&HW.Display)`), and a static initializer in
//  another translation unit may run before HW's constructor.  For a real
//  member `&HW.Display` is a link-time constant and stays valid; for a
//  reference member it would still read 0 from .bss and hand M5Canvas a
//  null parent, which blows up on the first pushSprite().
// ---------------------------------------------------------------------
class CrowPanel {
public:
    CrowPanelGfx      Display;                 // real member, see above
    m5::Touch_Class   Touch;
    m5::Button_Class  BtnA;
    KnobEncoder       Encoder;

    M5GFX& Lcd = Display;   // M5Dial naming, used by the standby hooks
    LedStrip Leds;          // the five addressable pixels

    void begin() {
        pinMode(HW_BTN_PIN, INPUT_PULLUP);

        Display.begin();              // our init_impl() does the wiring
        Display.setBrightness(HW_BACKLIGHT);
        Touch.begin(&Display);

        Leds.begin();
        // Started before the encoder so the two are not competing to be the
        // first task off the mark after boot.  The greeting blocks its own
        // task for about 1.5 s, not the loop: the encoder runs on its own
        // task and the UI has not started yet, so nothing is delayed.
        Leds.start();
        Encoder.begin(HW_ENC_A, HW_ENC_B);
    }

    void update() {
        const uint32_t msec = millis();
        BtnA.setRawState(msec, digitalRead(HW_BTN_PIN) == HW_BTN_LEVEL);
        Touch.update(msec);


#if CROWPANEL_KNOB_DEBUG
        // One short line per second, unconditionally, and it reports the
        // raw pin levels too.  A print that only fires on change cannot
        // distinguish "the knob was not turned" from "the decoder is not
        // running", which is exactly the ambiguity that made the previous
        // capture unreadable.  1 Hz is nowhere near the rate that flooded
        // USB CDC and tripped the task watchdog earlier.
        static uint32_t t0 = 0;
        KnobEncoder::stats_t st;
        Encoder.getStats(&st);
        if (msec - t0 >= 1000) {
            t0 = msec;
            Serial.printf("[knob] pos=%ld raw=%u dr=%u edges=%lu "
                          "counted=%lu bounce=%lu\n",
                          (long)st.position, (unsigned)st.raw,
                          (unsigned)st.rawDr, (unsigned long)st.edges,
                          (unsigned long)st.counted,
                          (unsigned long)st.bounce);
        }
#endif
    }
};

extern CrowPanel HW;

// ---- lifecycle hooks used by main.cpp ---------------------------------

static inline void hwBegin() {
    HW.begin();
    HW.BtnA.setHoldThresh(600);  // long-press = mode switch
}

// ---- display power -----------------------------------------------------
// Cutting the 3.3 V rails as well as the backlight is what actually makes
// the CrowPanel cheap in standby, so it is not just Lcd.sleep().
static inline void hwDisplaySleep() {
    HW.Lcd.sleep();
    pinMode(CROWPANEL_PIN_LCD_BL, OUTPUT);
    digitalWrite(CROWPANEL_PIN_LCD_BL, LOW);
    pinMode(CROWPANEL_PIN_LCD_PWR, OUTPUT);
    digitalWrite(CROWPANEL_PIN_LCD_PWR, LOW);
    pinMode(CROWPANEL_PIN_LED_PWR, OUTPUT);
    digitalWrite(CROWPANEL_PIN_LED_PWR, LOW);
}

static inline void hwDisplayWake() {
    pinMode(CROWPANEL_PIN_LCD_PWR, OUTPUT);
    digitalWrite(CROWPANEL_PIN_LCD_PWR, HIGH);
    pinMode(CROWPANEL_PIN_LED_PWR, OUTPUT);
    digitalWrite(CROWPANEL_PIN_LED_PWR, HIGH);
    pinMode(CROWPANEL_PIN_LCD_BL, OUTPUT);
    digitalWrite(CROWPANEL_PIN_LCD_BL, HIGH);
    HW.Lcd.wakeup();
}

// Pins the deep-poll standby glances at.  The panel is unpowered as well,
// otherwise a floating backlight enable would keep it glowing between
// two polls.
static inline void hwStandbyPollPrepare() {
    pinMode(HW_ENC_A, INPUT_PULLUP);
    pinMode(HW_ENC_B, INPUT_PULLUP);
    pinMode(HW_BTN_PIN, INPUT_PULLUP);
    hwDisplaySleep();
}

// ---- status LEDs -------------------------------------------------------
// Say goodbye before the device sleeps: a red dot circles, then off.
// Must be called before hwDisplaySleep(), which cuts the LED rail and
// leaves the pixels dead.  Returns 0 where there is nothing to drive.
static inline uint32_t hwLedShutdown() {
    return HW.Leds.ready() ? HW.Leds.shutdownTurn() : 0;
}

// Feed the device state into the ring.  A single byte store into the
// animation task's own variable, so this is safe to call from the UI loop
// with no locking and costs nothing when the state has not changed.
//
// No network wins over the transport state, because a chase or a breath
// would otherwise imply a healthy, connected device.
static inline void hwLedSetState(bool playing, bool paused, bool wifi, int volume) {
    if (!HW.Leds.ready()) return;
    HW.Leds.setVolume(volume);
    if (!wifi)              HW.Leds.setMode(HW.Leds.MODE_NOWIFI);
    else if (playing)       HW.Leds.setMode(HW.Leds.MODE_PLAY);
    else if (paused)        HW.Leds.setMode(HW.Leds.MODE_PAUSE);
    else                    HW.Leds.setMode(HW.Leds.MODE_OFF);
}
