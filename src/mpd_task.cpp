#include "mpd_task.h"

#include <WiFi.h>
#include <Preferences.h>

#include "MpdClient.h"
#include "Browse.h"
#include "Playlist.h"
#include "app.h"
#include "config.h"
#include "knx_ip.h"

// forward declarations -------------------------------------------------
static void doBrowse(uint8_t type, const String& arg, const String& arg2,
                     uint32_t reqSeq);
static void browseAct(uint8_t act, const String& uri);
static void selectInstance(int idx);

// ---------------------------------------------------------------------
SharedState gShared;
QueueHandle_t gCmdQueue = nullptr;
static TaskHandle_t s_mpdTask = nullptr;   // allow suspend during standby

// ---------------------------------------------------------------------
static MpdClient    s_mpd;
static MpdPlaylist  s_playlist;          // queue (owned by the task)
static SemaphoreHandle_t s_plMux;        // guards s_playlist
static volatile bool     s_plReloadReq   = false;
static uint32_t          s_lastPlTry     = 0;

// ---- MPD instances ---------------------------------------------------
static const MpdInstance s_instances[] = { MPD_INSTANCES };
static const int s_instanceCount =
    (int)(sizeof(s_instances) / sizeof(s_instances[0]));
static int s_curInst = 0;   // current room index
// Consecutive MPD connect failures; fallback hops to the next instance
// once this reaches s_mpdFallbackFails.
static int      s_mpdConnFails  = 0;
static bool     s_userRoomSel   = false;  // user picked a room this boot
static constexpr int s_mpdFallbackFails = 4;

int mpdInstanceCount() { return s_instanceCount; }
const char* mpdInstanceName(int i) {
    if (i < 0 || i >= s_instanceCount) return "";
    return s_instances[i].name;
}
int mpdCurrentInstance() { return s_curInst; }

// Songs loaded per queue page (~few KB each - keeps the Dial out of OOM
// even with queues of thousands of entries).
static constexpr uint32_t kQueuePage = 48;

// ---- browse state (also owned by the task) -------------------------
static MpdBrowseList s_list;             // dirs / tags / playlists
static MpdPlaylist   s_listSongs;        // song lists (albums / playlist songs)
// ---------------------------------------------------------------------
static SemaphoreHandle_t s_brMux;        // guards s_list + s_listSongs
static volatile bool     s_brReloadReq   = false;
static uint32_t          s_lastBrTry     = 0;

// ---- NTP --------------------------------------------------------------
static bool s_ntpInit = false;   // configTime() issued once after Wi-Fi

// ---------------------------------------------------------------------
// Stream <-> station-name cache (the queue only carries the stale live
// Title / icy Name; the EXTINF names live in listplaylistinfo).  Fed
// whenever a stored playlist is loaded or previewed; the queue view uses
// it to show the real station name.  Only http(s) URIs are kept (local
// files already carry their Title in the queue).
static constexpr uint32_t kExtinfCap = 48;
static String    s_eUri[kExtinfCap];
static String    s_eName[kExtinfCap];
static uint32_t  s_eCount = 0;
static uint32_t  s_eHead  = 0;
static SemaphoreHandle_t s_eMux = nullptr;

static void extinfPut(const String& uri, const String& name) {
    if (uri.length() == 0 || name.length() == 0) return;
    if (!uri.startsWith("http://") && !uri.startsWith("https://")) return;
    xSemaphoreTake(s_eMux, portMAX_DELAY);
    for (uint32_t i = 0; i < s_eCount; ++i)
        if (s_eUri[i] == uri) {
            s_eName[i] = name;
            xSemaphoreGive(s_eMux);
            return;
        }
    if (s_eCount < kExtinfCap) {
        s_eUri[s_eCount]  = uri;
        s_eName[s_eCount] = name;
        s_eCount++;
    } else {                            // full: FIFO eviction
        s_eUri[s_eHead]  = uri;
        s_eName[s_eHead] = name;
        s_eHead          = (s_eHead + 1) % kExtinfCap;
    }
    xSemaphoreGive(s_eMux);
}

// Copy every stream entry's display name (EXTINF Name, else Title) into
// the cache.  Call from the MPD task only.
static void extinfFromPlaylist(const MpdPlaylist& pl) {
    for (uint32_t i = 0; i < pl.count(); ++i) {
        MpdSong s;
        if (!pl.entry(i, s) || s.file.length() == 0) continue;
        const String& nm = s.name.length() ? s.name : s.title;
        if (nm.length()) extinfPut(s.file, nm);
    }
}

// ---------------------------------------------------------------------
// helpers to set boolean shared fields safely
static void setPl(bool v) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.mpd = v;
    xSemaphoreGive(gShared.mux);
}
static void setWifi(bool v) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.wifi = v;
    xSemaphoreGive(gShared.mux);
}

void setPlaylistMode(bool on) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.plMode = on;
    xSemaphoreGive(gShared.mux);
}

void setBrowseMode(bool on) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.brMode = on;
    xSemaphoreGive(gShared.mux);
}

static void setPlErr(uint8_t e) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.plErr = e;
    xSemaphoreGive(gShared.mux);
}

// ---------------------------------------------------------------------
static void refreshShared() {
    MpdStatus st;
    MpdSong   so;

    bool statusOk = s_mpd.status(st);
    bool songOk   = s_mpd.currentSong(so);

    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    if (statusOk) {
        gShared.status = st;
        gShared.statusGen++;
    }
    if (songOk) {
        if (gShared.song.id != so.id || gShared.song.title != so.title ||
            gShared.song.pos != so.pos || gShared.song.artist != so.artist)
            gShared.songGen++;
        gShared.song = so;
    }
    xSemaphoreGive(gShared.mux);
}

// ---------------------------------------------------------------------
// Switch the active MPD instance (room).  Pulls the plug so the main loop
// reconnects to the new port, and drops the whole cached queue/browse
// state (a different MPD has a different playlist).
static void selectInstance(int idx) {
    if (idx < 0 || idx >= s_instanceCount) return;
    s_curInst = idx;

    Preferences prefs;
    prefs.begin("mpd", false);
    prefs.putUChar("inst", (uint8_t)idx);
    prefs.end();

    s_mpd.begin(MPD_HOST, s_instances[idx].port, MPD_PASSWORD);
    s_mpd.disconnect();          // force reconnect on the new port
    s_mpdConnFails = 0;
    setPl(false);
    setPlErr(0);
    s_plReloadReq = true;

    xSemaphoreTake(s_plMux, portMAX_DELAY);
    s_playlist.clear();
    xSemaphoreGive(s_plMux);

    xSemaphoreTake(s_brMux, portMAX_DELAY);
    s_list.clear();
    s_listSongs.clear();
    xSemaphoreGive(s_brMux);
    s_brReloadReq = true;

    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.curInst        = (uint8_t)idx;
    gShared.plErr          = 0;
    gShared.brLoaded       = false;
    gShared.brReqType      = BR_NONE;
    gShared.mpd            = false;
    xSemaphoreGive(gShared.mux);

    Serial.printf("[inst] switched to %s (port %u)\n",
                  s_instances[idx].name, s_instances[idx].port);
}

// ---------------------------------------------------------------------
static void execCommand(const MpdCommand& c) {
    switch (c.type) {
        case CMD_PLAY_PAUSE: s_mpd.togglePlay(); break;
        case CMD_NEXT:       s_mpd.next(); break;
        case CMD_PREV:       s_mpd.prev(); break;
        case CMD_STOP:       s_mpd.stop(); break;
        case CMD_SET_VOL: {
            const int cur = [] {
                xSemaphoreTake(gShared.mux, portMAX_DELAY);
                int v = gShared.status.volume;
                xSemaphoreGive(gShared.mux);
                return v;
            }();
            if (cur < 0) break;
            s_mpd.setVolume(cur + (int)c.value);
            break;
        }
        case CMD_PLAY_INDEX:
            if (c.value >= 0) s_mpd.playPos((int)c.value);
            break;
        case CMD_SET_RANDOM:
            if (c.value < 0) {
                bool cur = [] {
                    xSemaphoreTake(gShared.mux, portMAX_DELAY);
                    bool r = gShared.status.random;
                    xSemaphoreGive(gShared.mux);
                    return r;
                }();
                s_mpd.setRandom(!cur);
            } else {
                s_mpd.setRandom(c.value != 0);
            }
            refreshShared();
            break;
        case CMD_SET_REPEAT: {
            bool next;
            if (c.value < 0) {
                bool cur = [] {
                    xSemaphoreTake(gShared.mux, portMAX_DELAY);
                    bool r = gShared.status.single;
                    xSemaphoreGive(gShared.mux);
                    return r;
                }();
                next = !cur;
            } else {
                next = (c.value != 0);
            }
            s_mpd.setRepeat(next);
            s_mpd.setSingle(next);
            refreshShared();
            break;
        }
        case CMD_CLEAR:
            s_mpd.sendCommand("clear");
            s_plReloadReq = true;
            refreshShared();
            break;
        case CMD_UPDATE_DB: {
            String p;
            xSemaphoreTake(gShared.mux, portMAX_DELAY);
            p = gShared.actArg;
            xSemaphoreGive(gShared.mux);
            if (s_mpd.updateDb(p))
                Serial.printf("[db] update requested: %s\n",
                              p.length() ? p.c_str() : "/");
            break;
        }
        case CMD_KNX_TOGGLE:
            knxToggle(c.value);
            break;
        case CMD_BROWSE: {
            xSemaphoreTake(gShared.mux, portMAX_DELAY);
            uint8_t  t = gShared.brReqType;
            String   a = gShared.brArg;
            String   b = gShared.brArg2;
            uint32_t s = gShared.brReqSeq;
            xSemaphoreGive(gShared.mux);
            doBrowse(t, a, b, s);
            break;
        }
        case CMD_ACT: {
            int act = c.value;
            String uri;
            xSemaphoreTake(gShared.mux, portMAX_DELAY);
            uri = gShared.actArg;
            xSemaphoreGive(gShared.mux);
            browseAct(act, uri);
            break;
        }
        case CMD_SET_INSTANCE:
            if (c.value >= 0 && c.value < s_instanceCount) {
                s_userRoomSel = true;   // remember: the user chose this room
                selectInstance((int)c.value);
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Commands that must keep working while the MPD connection is down:
// KNX device toggles and switching rooms.  Everything else needs MPD and
// is dropped (it cannot be served anyway while offline).
static void drainDisconnectedCommands() {
    MpdCommand c;
    while (xQueueReceive(gCmdQueue, &c, 0)) {
        if (c.type == CMD_KNX_TOGGLE)      knxToggle(c.value);
        else if (c.type == CMD_SET_INSTANCE) selectInstance((int)c.value);
    }
}

static bool drainCommands() {
    if (uxQueueMessagesWaiting(gCmdQueue) == 0) return false;
    if (s_mpd.idleActive()) s_mpd.endIdle();

    bool    any = false;
    int32_t volAcc  = 0;
    bool    haveVol = false;
    MpdCommand c;
    while (xQueueReceive(gCmdQueue, &c, 0)) {
        any = true;
        if (c.type == CMD_SET_VOL) {
            volAcc += c.value;
            haveVol = true;
        } else {
            execCommand(c);
        }
    }
    if (haveVol) {
        MpdCommand v;
        v.type  = CMD_SET_VOL;
        v.value = volAcc;
        execCommand(v);
    }
    return any;
}

// ---------------------------------------------------------------------
// playlist reload (queue view) - fetches a window around the selection
static void reloadPlaylist() {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    int32_t sel = gShared.plSel;
    int32_t tot = gShared.status.plength;
    xSemaphoreGive(gShared.mux);
    if (sel < 0) sel = 0;
    if (tot < 0) tot = 0;

    if (tot == 0) {
        xSemaphoreTake(s_plMux, portMAX_DELAY);
        s_playlist.clear();
        xSemaphoreGive(s_plMux);
        setPlErr(0);
        return;
    }

    uint32_t t0 = millis();
    // page fits the whole queue when small, else a centered window
    uint32_t page  = kQueuePage;
    if ((uint32_t)tot < page) page = (uint32_t)tot;
    uint32_t start = (uint32_t)sel - page / 2;
    if ((int32_t)start < 0) start = 0;
    if (start + page > (uint32_t)tot) start = (uint32_t)tot - page;

    xSemaphoreTake(s_plMux, portMAX_DELAY);
    bool ok = s_playlist.reloadPage(s_mpd, start, page, (uint32_t)tot);
    FetchErr err = s_mpd.lastFetchErr();
    xSemaphoreGive(s_plMux);
    setPlErr(ok ? 0 : (uint8_t)err);
    if (ok) {
        xSemaphoreTake(gShared.mux, portMAX_DELAY);
        gShared.plGen++;
        xSemaphoreGive(gShared.mux);
    }
    Serial.printf("[queue] ok=%d err=%d page=%u..%u/%d heap=%u %lums\n",
                  (int)ok, (int)err, (unsigned)start,
                  (unsigned)(start + page - 1), (int)tot,
                  (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() - t0));
}

// ---------------------------------------------------------------------
// browse loader: fills s_list / s_listSongs and writes to gShared
static const char* _slugTitle(const String& s, bool lastSeg) {
    static char buf[40];
    const char* p = s.c_str();
    if (lastSeg) {
        for (const char* q = p; *q; ++q)
            if (*q == '/') p = q + 1;
    }
    size_t n = strlen(p);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    return buf;
}

static void doBrowse(uint8_t type, const String& arg, const String& arg2,
                     uint32_t reqSeq) {
    bool     ok   = false;
    uint8_t  kind = BC_NONE;
    uint32_t cnt  = 0;
    char     title[40];
    title[0] = 0;

    xSemaphoreTake(s_brMux, portMAX_DELAY);
    s_list.clear();
    s_listSongs.clear();
    uint32_t t0 = millis();

    switch (type) {
    case BR_DIR: {
        ok = s_list.loadDir(s_mpd, arg);
        if (ok) { kind = BC_ITEMS; cnt = s_list.count(); }
        strlcpy(title, _slugTitle(arg, true), sizeof title);
        if (!title[0]) strlcpy(title, "/", sizeof title);
        break;
    }
    case BR_ARTISTS:
        ok = s_list.loadTags(s_mpd, "list albumartist", "AlbumArtist");
        if (!ok || s_list.count() == 0)
            ok = s_list.loadTags(s_mpd, "list artist", "Artist");
        if (ok) { kind = BC_ITEMS; cnt = s_list.count(); }
        strlcpy(title, "Artists", sizeof title);
        break;
    case BR_ALBUMS: {
        // MPD 0.23 quotes filter values, so multi-word artists/albums work.
        String cmd = "list album albumartist \"" + arg + "\"";
        ok = s_list.loadTags(s_mpd, cmd, "Album");
        if (!ok || s_list.count() == 0)
            ok = s_list.loadTags(s_mpd,
                                 String("list album artist \"") + arg + "\"",
                                 "Album");
        if (ok) { kind = BC_ITEMS; cnt = s_list.count(); }
        strlcpy(title, _slugTitle(arg, false), sizeof title);
        break;
    }
    case BR_SONGS: {
        String cmd = "find albumartist \"" + arg + "\" album \"" + arg2 + "\"";
        ok = s_listSongs.reloadCmd(s_mpd, cmd);
        if (ok && s_listSongs.count() == 0)
            ok = s_listSongs.reloadCmd(
                s_mpd,
                String("find artist \"") + arg + "\" album \"" + arg2 + "\"");
        if (ok) { kind = BC_SONGS; cnt = s_listSongs.count(); }
        strlcpy(title, _slugTitle(arg2, false), sizeof title);
        break;
    }
    case BR_PLISTS:
        ok = s_list.loadPlaylists(s_mpd);
        if (ok) { kind = BC_ITEMS; cnt = s_list.count(); }
        strlcpy(title, "Playlists", sizeof title);
        break;
    case BR_PLS_VIEW:
        ok = s_listSongs.reloadCmd(
            s_mpd,
            String("listplaylistinfo \"") + arg + "\"");
        if (ok) {
            kind = BC_SONGS; cnt = s_listSongs.count();
            extinfFromPlaylist(s_listSongs);   // stream EXTINF names
        }
        strlcpy(title, _slugTitle(arg, false), sizeof title);
        break;
    default: break;
    }

    xSemaphoreGive(s_brMux);

    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.brLoaded = ok;
    gShared.brErr    = ok ? 0 : (uint8_t)s_mpd.lastFetchErr();
    gShared.brKind   = kind;
    gShared.brCount  = cnt;
    strlcpy(gShared.brTitle, title, sizeof gShared.brTitle);
    gShared.brGen    = reqSeq;
    xSemaphoreGive(gShared.mux);

    Serial.printf("[browse] t=%d arg='%s' arg2='%s' ok=%d kind=%d cnt=%u "
                  "err=%d %lums heap=%u\n",
                  (int)type, arg.c_str(), arg2.c_str(), (int)ok, (int)kind,
                  (unsigned)cnt, s_mpd.lastFetchErr(), (unsigned long)(millis() - t0),
                  (unsigned)ESP.getFreeHeap());
}

// ---------------------------------------------------------------------
// browse action (enqueue / play / load playlist)
static void browseAct(uint8_t act, const String& uri) {
    switch (act) {
    case ACT_ENQUEUE:
        s_mpd.addUri(uri);
        break;
    case ACT_PLAY: {
        MpdStatus st;
        int before = 0;
        if (s_mpd.status(st)) before = st.plength;
        if (before < 0) before = 0;
        if (s_mpd.addUri(uri)) s_mpd.playPos(before);
        break;
    }
    case ACT_LOAD_PL: {
        if (s_mpd.loadPlaylist(uri)) {
            // The loaded queue only reports the stale live stream tags, so
            // re-read the stored playlist to cache the EXTINF names.
            MpdPlaylist pl;
            if (pl.reloadCmd(s_mpd, String("listplaylistinfo \"") + uri + "\""))
                extinfFromPlaylist(pl);
            s_mpd.playPos(0);
        }
        break;
    }
    }
    s_plReloadReq = true;  // queue changed, refresh
}

// =====================================================================
static uint32_t s_wifiLastTry = 0;
static bool     s_wifiEverTried = false;   // boot: first attempt must not wait

static void tryConnectWifi() {
    if (WiFi.status() == WL_CONNECTED) { setWifi(true); return; }

    // Cooldown between attempts: repeatedly calling WiFi.begin() while the
    // stack is mid-reconnect is a known ESP32 hammering bug that can leave
    // the station stuck.  Auto-reconnect (setAutoReconnect) keeps trying on
    // its own; here we only (re)arm it at most every 15 s when it's not
    // already running its state machine.
    if (s_wifiEverTried && (int32_t)(millis() - s_wifiLastTry) < 15000) return;
    s_wifiEverTried = true;
    s_wifiLastTry = millis();

    setWifi(false);
    Serial.printf("[wifi] attempt, status %d\n", (int)WiFi.status());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 3000) delay(200);
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("[wifi] connected (rssi %ddBm)\n", (int)WiFi.RSSI());
    else
        Serial.printf("[wifi] no link yet (status %d)\n", (int)WiFi.status());
    setWifi(WiFi.status() == WL_CONNECTED);
}

// =====================================================================
static void mpdTask(void*) {
    uint32_t mpdReconnectMs = 1000;
    uint32_t lastRef        = 0;

    for (;;) {
        // ---- 1. Wi-Fi ------------------------------------------------
        if (WiFi.status() != WL_CONNECTED) {
            knxReset();                     // drop the tunnel while offline
            if (s_mpd.isConnected()) s_mpd.disconnect();
            setPl(false);
            tryConnectWifi();
            if (WiFi.status() != WL_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        } else {
            setWifi(true);   // auto-reconnect can relink without our help
        }

        // ---- 1b. NTP once ---------------------------------------------
        if (!s_ntpInit) {
            s_ntpInit = true;
            configTime((long)TIMEZONE_UTC_HOURS * 3600, 0, NTP_SERVER);
        }

        // ---- 1c. KNX tunnel (non-blocking) -----------------------------
        knxLoop();

        // ---- 2. MPD connection ----------------------------------------
        if (!s_mpd.isConnected()) {
            bool ok = s_mpd.connect();
            setPl(ok);
            if (ok)
                Serial.printf("[mpd] connected to %s:%d (\"%s\")\n", MPD_HOST,
                              s_instances[s_curInst].port,
                              s_instances[s_curInst].name);
            else
                Serial.printf("[mpd] connect to %s:%d FAILED (\"%s\")\n",
                              MPD_HOST, s_instances[s_curInst].port,
                              s_instances[s_curInst].name);
            if (!ok) {
                drainDisconnectedCommands();   // serve KNX / room-change now
                vTaskDelay(pdMS_TO_TICKS(mpdReconnectMs));
                if (mpdReconnectMs < 10000) mpdReconnectMs *= 2;
                // Self-heal: if the saved room cannot be reached (e.g. its
                // MPD is wedged like the 6601 instance), hop to the next
                // instance instead of sitting on a red MPD dot forever.
                // Only automatic while the user hasn't explicitly picked a
                // room in this session.
                if (!s_userRoomSel &&
                    ++s_mpdConnFails >= s_mpdFallbackFails) {
                    s_mpdConnFails = 0;
                    int next = (s_curInst + 1) % s_instanceCount;
                    Serial.printf("[mpd] %d fails on \"%s\" - trying "
                                  "\"%s\" next\n",
                                  s_mpdFallbackFails,
                                  s_instances[s_curInst].name,
                                  s_instances[next].name);
                    selectInstance(next);
                }
                continue;
            }
            mpdReconnectMs = 1000;
            s_mpdConnFails = 0;
            refreshShared();
            s_mpd.startIdle();
            lastRef = millis();
            continue;
        }

        // ---- 3. UI commands -------------------------------------------
        bool didCmd = drainCommands();
        if (didCmd || !s_mpd.idleActive()) refreshShared();
        if (!s_mpd.idleActive()) s_mpd.startIdle();

        // ---- 4. playlist reload on demand -----------------------------
        {
            xSemaphoreTake(gShared.mux, portMAX_DELAY);
            bool    plMode = gShared.plMode;
            int32_t plSel  = gShared.plSel;
            xSemaphoreGive(gShared.mux);

            // The queue view wants a page around "sel".  reload always hops
            // to the requested page (even if a page is already loaded), so
            // scrolling near an edge just re-centers the window.
            if (s_plReloadReq || plMode) {
                xSemaphoreTake(s_plMux, portMAX_DELAY);
                bool     loaded  = s_playlist.isValid();
                uint32_t pageSt  = s_playlist.pageStart();
                uint32_t pageCnt = s_playlist.count();
                xSemaphoreGive(s_plMux);

                bool want = s_plReloadReq;
                if (loaded && !want) {
                    // recentre only when the selection leaves the loaded page
                    if (plSel < (int32_t)pageSt ||
                        plSel >= (int32_t)(pageSt + pageCnt))
                        want = true;
                } else if (!loaded && plMode) {
                    want = true;   // first load / reload after failure
                }
                if (want) {
                    if (s_mpd.idleActive()) s_mpd.endIdle();
                    s_plReloadReq = false;
                    if (millis() - s_lastPlTry >= 300) {
                        s_lastPlTry = millis();
                        reloadPlaylist();
                    }
                }
            }
        }

        // ---- 4b. browse reload on demand ------------------------------
        {
            xSemaphoreTake(gShared.mux, portMAX_DELAY);
            bool    brMode   = gShared.brMode;
            bool    brLoaded = gShared.brLoaded;
            uint8_t reqType  = gShared.brReqType;
            String  a        = gShared.brArg;
            String  b        = gShared.brArg2;
            uint32_t seq     = gShared.brReqSeq;
            xSemaphoreGive(gShared.mux);

            if (brMode && (s_brReloadReq || !brLoaded) && reqType != BR_NONE) {
                if (s_mpd.idleActive()) s_mpd.endIdle();
                s_brReloadReq = false;
                if (millis() - s_lastBrTry >= 2000) {
                    s_lastBrTry = millis();
                    doBrowse(reqType, a, b, seq);
                }
            }
        }

        // ---- 5. non-blocking idle poll --------------------------------
        String subs = s_mpd.pollIdle();
        if (subs.length()) {
            refreshShared();
            if (subs.indexOf("playlist") >= 0 || subs.indexOf("player") >= 0)
                s_plReloadReq = true;
            if (subs.indexOf("stored_playlist") >= 0 ||
                subs.indexOf("database") >= 0 ||
                subs.indexOf("update") >= 0)
                s_brReloadReq = true;
        }

        // ---- 6. periodic resync ---------------------------------------
        if (millis() - lastRef >= 3000) {
            if (s_mpd.idleActive()) s_mpd.endIdle();
            refreshShared();
            lastRef = millis();
        }

        if (!s_mpd.idleActive() && s_mpd.isConnected()) s_mpd.startIdle();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
static void logFetchCap() {
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t cap   = (psram > 256 * 1024) ? psram / 2 : 120 * 1024;
    Serial.printf("[mpd] total_psram=%u free_psram=%u fetch_cap=%u\n",
                  (unsigned)psram,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)cap);
}
void startMpdTask() {
    if (s_plMux == nullptr) s_plMux = xSemaphoreCreateMutex();
    if (s_brMux == nullptr) s_brMux = xSemaphoreCreateMutex();
    if (s_eMux == nullptr)  s_eMux  = xSemaphoreCreateMutex();

    // Restore the last selected room, then connect to its port right away.
    Preferences prefs;
    prefs.begin("mpd", false);
    s_curInst = (int)prefs.getUChar("inst", 0);
    prefs.end();
    if (s_curInst < 0 || s_curInst >= s_instanceCount) s_curInst = 0;
    gShared.curInst = (uint8_t)s_curInst;

    s_mpd.begin(MPD_HOST, s_instances[s_curInst].port, MPD_PASSWORD);
    s_mpdConnFails = 0;
    s_userRoomSel  = false;
    Serial.printf("[mpd] instance %d = \"%s\" (port %d)\n", s_curInst,
                  s_instances[s_curInst].name, s_instances[s_curInst].port);
    knxSetup();
    logFetchCap();
    xTaskCreatePinnedToCore(mpdTask, "mpd", 8192, nullptr, 2, &s_mpdTask, 0);
}

// ---------------------------------------------------------------------
// Freeze / un-freeze the whole network task while the M5Dial is in
// standby (light sleep).  Freezing before Wi-Fi is turned off guarantees
// the task cannot re-enable Wi-Fi behind our back during the power-down.
void mpdSetStandby(bool on) {
    if (on) {
        if (s_mpdTask) vTaskSuspend(s_mpdTask);
    } else {
        if (s_mpdTask) vTaskResume(s_mpdTask);
    }
}

// ---------------------------------------------------------------------
void requestPlaylistReload() { s_plReloadReq = true; }

const MpdPlaylist* mpdPlaylistHandle()  { return &s_playlist; }
void mpdPlaylistLock()   { xSemaphoreTake(s_plMux, portMAX_DELAY); }
void mpdPlaylistUnlock() { xSemaphoreGive(s_plMux); }

const MpdBrowseList* mpdBrowseHandle()      { return &s_list; }
const MpdPlaylist*   mpdBrowseSongsHandle() { return &s_listSongs; }
void mpdBrowseLock()   { xSemaphoreTake(s_brMux, portMAX_DELAY); }
void mpdBrowseUnlock() { xSemaphoreGive(s_brMux); }

// ---------------------------------------------------------------------
// Thread-safe lookup of the stored-playlist EXTINF name for a stream URL.
// Returns "" when the URL is not a cached radio stream.
String mpdExtinfName(const String& file) {
    if (file.length() == 0) return String();
    String hit;
    xSemaphoreTake(s_eMux, portMAX_DELAY);
    for (uint32_t i = 0; i < s_eCount; ++i)
        if (s_eUri[i] == file) {
            hit = s_eName[i];
            break;
        }
    xSemaphoreGive(s_eMux);
    return hit;
}