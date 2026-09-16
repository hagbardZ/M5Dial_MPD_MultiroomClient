#pragma once

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "MpdClient.h"

// ---------------------------------------------------------------------
// Commands posted by the UI towards the MPD task
enum MpdCmdType : uint8_t {
    CMD_PLAY_PAUSE = 0,
    CMD_NEXT,
    CMD_PREV,
    CMD_STOP,
    CMD_SET_VOL,
    CMD_PLAY_INDEX,
    CMD_BROWSE,   // value = BrType; target in gShared.brArg / brArg2
    CMD_ACT,      // value = BrowseAct; uri in gShared.actArg
    CMD_SET_INSTANCE,  // value = MPD instance index (switch room/port)
    CMD_SET_RANDOM,    // value = 0=off, 1=on, -1=toggle
    CMD_SET_REPEAT,    // value = 0=off, 1=on, -1=toggle
    CMD_CLEAR,         // clear the queue
};

struct MpdCommand {
    uint8_t type;
    int32_t value;
};

// ---------------------------------------------------------------------
// What the browser should load (the "top frame" of the UI stack).
enum BrType : uint8_t {
    BR_NONE = 0,
    BR_DIR,        // arg   = directory uri ("" = music root)
    BR_ARTISTS,    // no args
    BR_ALBUMS,     // arg   = artist
    BR_SONGS,      // arg   = artist, arg2 = album
    BR_PLISTS,     // no args (stored playlists)
    BR_PLS_VIEW,   // arg   = playlist name
};

// Which holder the MPD task filled after a BR_* load.
enum BrContent : uint8_t { BC_NONE = 0, BC_ITEMS, BC_SONGS };

// Actions available on a browsed item.
enum BrowseAct : uint8_t {
    ACT_ENQUEUE = 0,
    ACT_PLAY,      // add then play (single file, directory or .m3u)
    ACT_LOAD_PL,   // load a stored playlist and play it
};

enum { BROWSE_ARG_MAX = 256, BROWSE_ARG2_MAX = 128, ACT_ARG_MAX = 256 };

// ---------------------------------------------------------------------
// State shared between the MPD task (writer) and the UI task (reader).
// Access is serialized through gShared.mux - keep copies small.
struct SharedState {
    SemaphoreHandle_t mux = nullptr;
    MpdStatus status;
    MpdSong   song;
    bool      wifi = false;
    bool      mpd  = false;
    uint8_t   curInst = 0;  // active MPD instance index (room)
    bool      plMode = false;   // UI is showing the queue view
    uint8_t   plErr  = 0;       // FetchErr of the last playlist load (0 = ok)
    int32_t   plSel  = 0;         // absolute queue pos the UI wants centered
    uint32_t  statusGen = 0;   // bumped on every refresh
    uint32_t  songGen   = 0;   // bumped when the current song changes
    uint32_t  plGen     = 0;   // bumped when the playlist is (re)loaded

    // ---- browser bridge --------------------------------------------
    bool      brMode    = false;   // UI is inside the browser (retry loads)
    bool      brLoaded  = false;   // current frame content loaded OK
    uint8_t   brReqType = BR_NONE; // BrType being requested
    uint8_t   brErr     = 0;       // FetchErr of the last browse load
    uint8_t   brKind    = BC_NONE; // holder populated by the task
    uint32_t  brCount   = 0;
    uint32_t  brReqSeq  = 0;       // incremented by the UI per request
    uint32_t  brGen     = 0;       // mirrored to brReqSeq when loaded
    char      brTitle[40]      = {0};
    char      brArg[BROWSE_ARG_MAX]  = {0};
    char      brArg2[BROWSE_ARG2_MAX] = {0};
    // ---- action bridge ---------------------------------------------
    char      actArg[ACT_ARG_MAX] = {0};
};

extern SharedState gShared;
extern QueueHandle_t gCmdQueue;

// ---------------------------------------------------------------------
// Small UI->task helpers: set the shared fields, then the UI posts the
// corresponding command (the mutex release happens before the queue write,
// so the task always sees a consistent snapshot).
static inline void browseReq1(uint8_t type, const char* arg) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.brReqType = type;
    if (!arg) arg = "";
    strlcpy(gShared.brArg, arg, sizeof gShared.brArg);
    gShared.brArg2[0] = 0;
    xSemaphoreGive(gShared.mux);
}

static inline void browseReq2(uint8_t type, const char* arg,
                              const char* arg2) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.brReqType = type;
    if (!arg) arg = "";
    if (!arg2) arg2 = "";
    strlcpy(gShared.brArg, arg, sizeof gShared.brArg);
    strlcpy(gShared.brArg2, arg2, sizeof gShared.brArg2);
    xSemaphoreGive(gShared.mux);
}

static inline void actRequest(const char* uri) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    if (uri) strlcpy(gShared.actArg, uri, sizeof gShared.actArg);
    else gShared.actArg[0] = 0;
    xSemaphoreGive(gShared.mux);
}