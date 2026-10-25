#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>
#include "soc/soc_caps.h"

#define USB_VID 0x303a
#define USB_PID 0x1001

// ---- console (USB-CDC) + external UART header ------------------------
static const uint8_t TX = 43;
static const uint8_t RX = 44;

static const uint8_t TXD2 = 43;
static const uint8_t RXD2 = 44;

// ---- panel (GC9A01, 4-wire SPI) -------------------------------------
static const uint8_t SS = 9;
static const uint8_t MOSI = 11;
static const int8_t  MISO = -1;   // the panel has no MISO
static const uint8_t SCK = 10;
static const uint8_t DC = 3;
static const uint8_t LCD_RST = 14;
static const uint8_t LCD_BL = 46;
static const uint8_t LCD_PWR = 1;   // LCD_3V3 rail enable (HIGH = on)
static const uint8_t LCD_PWR_A = 2;  // LEDA_3V3 rail enable (HIGH = on)

// ---- touch (CST816D, I2C @ 0x15) -------------------------------------
static const uint8_t SDA = 6;
static const uint8_t SCL = 7;
static const uint8_t TP_INT = 5;
static const uint8_t TP_RST = 13;

// ---- rotary knob (quadrature + push switch, active low) -------------
static const uint8_t ENCODER_A = 45;
static const uint8_t ENCODER_B = 42;
static const uint8_t ENCODER_SW = 41;

// ---- board buttons / LEDs -------------------------------------------
static const uint8_t BOOT = 0;      // BOOT button, active low
static const uint8_t LED_RED = 40;  // power indicator
static const uint8_t LED_RGB = 48;  // WS2812 strip data (5 pixels)

// ---- free expansion header ------------------------------------------
static const uint8_t IO4 = 4;
static const uint8_t IO12 = 12;
static const uint8_t SDA2 = 38;
static const uint8_t SCL2 = 39;

// ---- loose GPIO mirrors (Arduino convention) -------------------------
static const uint8_t G0 = 0;
static const uint8_t G1 = 1;
static const uint8_t G2 = 2;
static const uint8_t G3 = 3;
static const uint8_t G4 = 4;
static const uint8_t G5 = 5;
static const uint8_t G6 = 6;
static const uint8_t G7 = 7;
static const uint8_t G8 = 8;
static const uint8_t G9 = 9;
static const uint8_t G10 = 10;
static const uint8_t G11 = 11;
static const uint8_t G12 = 12;
static const uint8_t G13 = 13;
static const uint8_t G14 = 14;
static const uint8_t G15 = 15;
static const uint8_t G38 = 38;
static const uint8_t G39 = 39;
static const uint8_t G40 = 40;
static const uint8_t G41 = 41;
static const uint8_t G42 = 42;
static const uint8_t G43 = 43;
static const uint8_t G44 = 44;
static const uint8_t G45 = 45;
static const uint8_t G46 = 46;
static const uint8_t G48 = 48;

static const uint8_t ADC1 = 4;
static const uint8_t ADC2 = 12;

#endif /* Pins_Arduino_h */
