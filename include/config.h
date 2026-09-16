#pragma once

// =====================================================================
//  M5Dial MPD client - configuration
//
//  Adjust the values below to match your network and MPD setup, then
//  build with `pio run` and flash with `pio run -t upload`.
// =====================================================================

// ---- Wi-Fi -----------------------------------------------------------
#define WIFI_SSID "yourSSID"
#define WIFI_PASS "YOUR_PWD"

// ---- MPD server ------------------------------------------------------
// IP address or hostname of the machine running MPD (port 6600).
#define MPD_HOST "192.168.1.10"
#define MPD_PORT 6601
// Optional MPD password (leave empty if MPD has no password set).
#define MPD_PASSWORD ""

// ---- MPD instances ---------------------------------------------------
// One MPD per room, each on its own port.  Define the port for each room,
// then add both the on-screen name and the define to MPD_INSTANCES below.
#define Room1           6600
#define Room2        6601
#define Room3      6603
#define Room4              6602
#define Room5          6607
#define Room6            6605
#define Room7          6604
#define Room8            6606

// List shown in the menu: { display name, port define }.
// The first entry is the default instance used at boot.
#define MPD_INSTANCES                        \
    { "Lab", Room1 },                   \
    { "Wohnzimmer", Room2 },            \
    { "Schlafzimmer", Room3 },        \
    { "Bad", Room4 },                         \
    { "Marius", Room5 },                  \
    { "Buero", Room6 },                     \
    { "Keller", Room7 },                  \
    { "AlleRaeume", Room8 }
