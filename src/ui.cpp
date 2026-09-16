#include "ui.h"

#include <string.h>

#include <M5Dial.h>

#include "Browse.h"
#include "Playlist.h"
#include "app.h"
#include "mpd_task.h"

// ---------------------------------------------------------------------
// Round-screen metrics: 240x240, centre at (120,120), radius 120.
// Content lives inside the circle; the enclosing corners are never
// visible on the GC9A01 panel.
// ---------------------------------------------------------------------
static constexpr int CX = 120;
static constexpr int CY = 120;

// progress ring band
static constexpr int R_OUTER = 114;
static constexpr int R_INNER = 106;

// max horizontal width for centered text (kept clear of the ring)
static constexpr int TXT_W = 128;

enum Mode : uint8_t {
    MODE_NOW,
    MODE_MENU,
    MODE_QUEUE,
    MODE_BROWSE,
    MODE_INSTS,
    MODE_PLAYMENU,
};

// play-menu rows
enum PlayRow : uint8_t { PLAY_RANDOM = 0, PLAY_REPEAT, PLAY_CLEAR, PLAY_ROWS };

// ---------------------------------------------------------------------
// Browser navigation stack.  The top frame is loaded by the MPD task and
// determines what we draw; push = drill in, pop = back.
enum class Fb : uint8_t {
    DIR,      // lsinfo listing
    ARTISTS,  // list albumartist
    ALBUMS,   // list album of an artist (arg = artist)
    SONGS,    // find songs of an album (arg = artist, arg2 = album)
    PLISTS,   // listplaylists
    PLS_VIEW, // listplaylistinfo (arg = playlist)
};

struct BFrame {
    Fb     type;
    int    sel;
    String arg;
    String arg2;
};

// ---------------------------------------------------------------------
static M5Canvas spr(&M5Dial.Display);   // PSRAM double buffer
static Mode  s_mode     = MODE_NOW;
static int   s_songSel  = 0;             // selection in the queue view
static int   s_menuSel  = 0;             // selection in the main menu
static int   s_instSel  = 0;             // selection in the instance picker
static int   s_playSel  = 0;             // selection in the play menu
static bool  s_dirty    = true;
static Mode  s_lastMode = Mode(0xFF);    // force first draw
static int   s_encAcc   = 0;             // encoder jitter accumulator (NOW mode)

static BFrame s_stack[12];
static int    s_depth = 0;               // # of frames (top = s_depth-1)

static uint32_t s_reqSeq = 0;            // browse request sequence
static struct {
    bool     armed;
    uint32_t idx;
    uint8_t  depth;
    uint8_t  type;
} s_pend;

// short confirmation bubble after a browser action
static String    s_toast;
static uint32_t  s_toastUntil = 0;
static uint16_t  s_toastCol   = 0;

// arrow flash feedback in now view (0=off, 1=prev, 2=next)
static uint8_t   s_arrowFlash = 0;
static uint32_t  s_arrowUntil = 0;

static void showToast(const char* s, uint16_t col) {
    s_toast     = s ? s : "";
    s_toastCol  = col;
    s_toastUntil = millis() + 2200;
}

static const char* s_menuItems[6] = {"Queue", "Files", "Library", "Playlists",
                                     "change room", "Back"};

// colours
static uint16_t cBg, cRing, cTrack, cProgress, cText, cDim, cOk, cBad,
    cWarn, cSel, cHighlight;

// fonts
static const lgfx::IFont* s_fTitle = &fonts::DejaVu18;
static const lgfx::IFont* s_fSmall = &fonts::DejaVu12;
static const lgfx::IFont* s_fTime  = &fonts::Orbitron_Light_24;

static uint32_t s_drawSig = 0;

// ---------------------------------------------------------------------
static void post(uint8_t type, int32_t value = 0) {
    MpdCommand c;
    c.type  = type;
    c.value = value;
    if (gCmdQueue) xQueueSend(gCmdQueue, &c, 0);
}

// ---------------------------------------------------------------------
static BFrame& topFrame() { return s_stack[s_depth - 1]; }

// safe accessor for the signature computation (mode may not be BROWSE)
static uint32_t topSelSig() {
    if (s_mode == MODE_BROWSE && s_depth > 0) return (uint32_t)topFrame().sel;
    return 0;
}

static void enterBrowse(Fb t, const String& arg);
static void leaveBrowse();

static int  windowTop(int sel, uint32_t count);

static BrType brOf(Fb t) {
    switch (t) {
        case Fb::DIR:      return BR_DIR;
        case Fb::ARTISTS:  return BR_ARTISTS;
        case Fb::ALBUMS:   return BR_ALBUMS;
        case Fb::SONGS:    return BR_SONGS;
        case Fb::PLISTS:   return BR_PLISTS;
        case Fb::PLS_VIEW: return BR_PLS_VIEW;
    }
    return BR_NONE;
}

// ---------------------------------------------------------------------
static void syncModes() {
    setPlaylistMode(s_mode == MODE_QUEUE);
    setBrowseMode(s_mode == MODE_BROWSE);
}

// ---------------------------------------------------------------------
// Snap one SharedState.  Used by both drawing and input handlers.
static void snapShared(SharedState& out) {
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    out = gShared;
    xSemaphoreGive(gShared.mux);
}

// ---------------------------------------------------------------------
static void browsePost() {
    const BFrame& f = topFrame();
    ++s_reqSeq;
    xSemaphoreTake(gShared.mux, portMAX_DELAY);
    gShared.brReqSeq  = s_reqSeq;
    gShared.brLoaded  = false;
    gShared.brReqType = (uint8_t)brOf(f.type);
    strlcpy(gShared.brArg, f.arg.c_str(), sizeof gShared.brArg);
    strlcpy(gShared.brArg2, f.arg2.c_str(), sizeof gShared.brArg2);
    xSemaphoreGive(gShared.mux);
    post(CMD_BROWSE, (int32_t)brOf(f.type));
    s_pend.armed = false;
    s_dirty      = true;
}

static void pushFrame(Fb t, const String& arg, const String& arg2 = "") {
    if (s_depth < (int)(sizeof s_stack / sizeof s_stack[0])) {
        BFrame& f = s_stack[s_depth];
        f.type = t;
        f.sel  = 0;
        f.arg  = arg;
        f.arg2 = arg2;
        s_depth++;
        browsePost();
    }
}

static void popFrame() {
    if (s_depth > 1) {
        --s_depth;              // parent frame keeps its cached selection
        browsePost();
    } else {
        leaveBrowse();
    }
}

static void enterBrowse(Fb t, const String& arg) {
    s_mode  = MODE_BROWSE;
    s_depth = 0;
    pushFrame(t, arg);
    syncModes();
    s_dirty = true;
}

static void leaveBrowse() {
    s_depth = 0;
    s_mode  = MODE_MENU;
    syncModes();
    s_dirty = true;
}

// ---------------------------------------------------------------------
void uiInit() {
    spr.setColorDepth(16);
    if (!spr.createSprite(240, 240)) {
        // PSRAM unavailable -> fall back to internal RAM
        spr.setPsram(false);
        if (!spr.createSprite(240, 240)) {
            while (true) delay(1000);  // no framebuffer possible - halt
        }
    }
    spr.setTextWrap(false);
    spr.setTextDatum(middle_center);

    cBg        = spr.color565(14, 19, 28);
    cRing      = spr.color565(36, 46, 60);
    cTrack     = spr.color565(70, 80, 96);
    cProgress  = spr.color565(242, 188, 64);
    cText      = spr.color565(236, 241, 246);
    cDim       = spr.color565(128, 138, 154);
    cOk        = spr.color565(96, 200, 128);
    cBad       = spr.color565(226, 88, 88);
    cWarn      = spr.color565(236, 196, 76);
    cSel       = spr.color565(34, 48, 68);
    cHighlight = spr.color565(242, 188, 64);
}

// ---------------------------------------------------------------------
static void setFont(const lgfx::IFont* f) {
    spr.setFont(f);
    spr.setTextSize(1);
}

static void drawText(int x, int y, uint16_t color, const char* s) {
    spr.setTextColor(color);
    spr.drawString(s, x, y);
}

static void fmtTime(int sec, char* buf, size_t n) {
    if (sec < 0) sec = 0;
    snprintf(buf, n, "%d:%02d", sec / 60, sec % 60);
}

// ---------------------------------------------------------------------
// Chop one whole UTF-8 character sitting just before the trailing "...".
// The font is ASCII-only, so we append "..." (not U+2026) to avoid a
// missing-glyph box.
static void chopBeforeEllipsis(String& s) {
    int end = (int)s.length() - 3;  // region before the "..."
    if (end <= 0) return;
    int i = end;
    while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) --i;  // continuations
    if (i == end) i = end - 1;      // single-byte char: remove at least 1
    s.remove(i, end - i);
}

// Truncate a string to fit within maxPx pixels (current font).
static String truncate(const String& s, int maxPx) {
    if (spr.textWidth(s) <= maxPx) return s;
    String s2 = s + "...";          // ASCII ellipsis (font is ASCII-only)
    unsigned guard = 80;            // safety net against any stuck loop
    while (spr.textWidth(s2) > maxPx && s2.length() > 3 && (guard-- > 0))
        chopBeforeEllipsis(s2);
    return s2;
}

// ---------------------------------------------------------------------
// Scrolling marquee for content wider than the visible band.
static void drawMarquee(const String& text, int y, uint16_t color,
                        const lgfx::IFont* f) {
    setFont(f);
    int w = (int)spr.textWidth(text);
    if (w <= TXT_W + 2) {
        drawText(CX, y, color, text.c_str());
        return;
    }
    const int  SP    = 28;
    const int  cycle = w + SP;
    const int  phase = (millis() / 40) % cycle;
    for (int i = -2; i <= 3; ++i) {
        const int cx = CX - phase + i * cycle;
        if (cx + w / 2 < -3) continue;
        if (cx - w / 2 > 243) break;
        drawText(cx, y, color, text.c_str());
    }
}

// ---------------------------------------------------------------------
static void drawStatusDots(bool wifi, bool mpd, bool active) {
    const int x[3] = {102, 120, 138};
    for (int i = 0; i < 3; ++i) spr.fillCircle(x[i], 20, 6, cBg);
    for (int i = 0; i < 3; ++i) spr.fillCircle(x[i], 20, 3, cRing);
    spr.fillCircle(x[0], 20, 3, wifi ? cOk : cBad);
    spr.fillCircle(x[1], 20, 3, mpd ? cOk : cBad);
    spr.fillCircle(x[2], 20, 3, active ? cWarn : cDim);
}

// ---------------------------------------------------------------------
static void drawProgressRing(float frac, uint16_t fill, uint16_t track) {
    spr.fillArc(CX, CY, R_OUTER, R_INNER, 0.0f, 360.0f, track);
    if (frac < 0.001f) return;
    if (frac > 1.0f) frac = 1.0f;
    if (frac >= 0.999f) {  // full circle (start==end would draw nothing)
        spr.fillArc(CX, CY, R_OUTER, R_INNER, 0.0f, 360.0f, fill);
        return;
    }
    // ring starts at 12 o'clock (-90°), sweeps clockwise
    spr.fillArc(CX, CY, R_OUTER, R_INNER, -90.0f, -90.0f + 360.0f * frac, fill);
}

// ---------------------------------------------------------------------
static String displayTitle(const MpdSong& song) {
    if (song.title.length() > 0) return song.title;
    if (song.file.length() > 0) {
        int l = song.file.length();
        while (l > 0 && song.file[l - 1] != '/') --l;
        return song.file.substring(l);
    }
    return "no track";
}

static String displayTitle(const MpdSong& song, const String& fallback) {
    if (song.title.length() > 0) return song.title;
    if (fallback.length() > 0) return fallback;
    return displayTitle(song);
}

// ---------------------------------------------------------------------
static void drawNowView(const SharedState& snap) {
    const MpdStatus st   = snap.status;
    const MpdSong   song = snap.song;

    spr.fillScreen(cBg);

    // ---- progress ring (smooth elapsed) -----------------------------
    static uint32_t s_lastGen;
    static uint32_t s_elRef;
    static int      s_elBase;

    int  showEl  = st.elapsed;
    bool playing = st.playing;
    if (playing) {
        if (snap.statusGen != s_lastGen) {
            s_lastGen = snap.statusGen;
            s_elBase  = st.elapsed;
            s_elRef   = millis();
        }
        showEl = s_elBase + (int)((millis() - s_elRef) / 1000);
        if (st.duration > 0 && showEl > st.duration) showEl = st.duration;
    } else {
        s_lastGen = 0xFFFFFFFF;  // resync on next play
    }

    float frac = 0.0f;
    if (st.duration > 0) frac = (float)showEl / (float)st.duration;
    drawProgressRing(frac, cProgress, cTrack);

    // ---- time (top) ------------------------------------------------
    char t[16];
    setFont(s_fTime);
    fmtTime(showEl, t, sizeof t);
    drawText(CX, 48, cText, t);
    if (st.duration > 0) {
        setFont(s_fSmall);
        fmtTime(st.duration, t, sizeof t);
        String dur = "/ ";
        dur += t;
        drawText(CX, 64, cDim, dur.c_str());
    }

    // ---- artist -----------------------------------------------------
    setFont(s_fSmall);
    drawText(CX, 92, cDim, truncate(song.artist, TXT_W).c_str());

    // ---- title (centre of the screen, marquee if too wide) ----------
    String title = displayTitle(song);
    setFont(s_fTitle);
    if (spr.textWidth(title) > TXT_W + 2) {
        drawMarquee(title, 120, cText, s_fTitle);
    } else {
        drawText(CX, 120, cText, title.c_str());
    }

    // ---- album ------------------------------------------------------
    setFont(s_fSmall);
    drawText(CX, 148, cDim, truncate(song.album, TXT_W).c_str());

    // ---- state ------------------------------------------------------
    setFont(s_fSmall);
    const char* stateStr = "stopped";
    uint16_t    stateCol = cDim;
    if (st.playing) {
        stateStr = "playing";
        stateCol = cOk;
    } else if (st.paused) {
        stateStr = "paused";
        stateCol = cWarn;
    }
    drawText(CX, 178, stateCol, stateStr);

    // ---- prev/next arrows beside "playing" ---------------------------
    {
        bool flashL = (s_arrowFlash == 1 && millis() < s_arrowUntil);
        bool flashR = (s_arrowFlash == 2 && millis() < s_arrowUntil);
        uint16_t colL = flashL ? cHighlight : cRing;
        uint16_t colR = flashR ? cHighlight : cRing;
        // left arrow: triangle pointing left, centred at (34, 178)
        spr.fillTriangle(24, 178, 44, 168, 44, 188, colL);
        // right arrow: triangle pointing right, centred at (206, 178)
        spr.fillTriangle(216, 178, 196, 168, 196, 188, colR);
    }

    // ---- repeat / random symbols beside "playing" ---------------------
    {
        int xsym = 156;   // x of first symbol (just right of state word)
        int ysym = 178;

        // repeat: small ring arc (3/4 circle) with arrowhead
        if (st.repeat) {
            spr.drawCircle(xsym, ysym, 5, cOk);
            spr.drawCircle(xsym, ysym, 4, cOk);
            // arrowhead pointing clockwise (tip at top-right of ring)
            spr.fillTriangle(xsym + 2, ysym - 7, xsym - 2, ysym - 4,
                             xsym + 3, ysym - 4, cOk);
        }

        // random: two small crossing arrows
        if (st.random) {
            int rx = xsym + 24;   // second symbol offset
            // top-left to bottom-right arrow
            spr.drawLine(rx - 6, ysym - 6, rx + 4, ysym + 4, cOk);
            spr.fillTriangle(rx + 4, ysym + 6, rx, ysym + 2, rx + 6, ysym + 2,
                             cOk);
            // bottom-left to top-right arrow
            spr.drawLine(rx - 6, ysym + 6, rx + 4, ysym - 4, cOk);
            spr.fillTriangle(rx + 4, ysym - 6, rx, ysym - 2, rx + 6, ysym - 2,
                             cOk);
        }
    }

    // ---- volume -----------------------------------------------------
    if (st.volume >= 0) {
        setFont(s_fSmall);
        String v = "vol ";
        v += st.volume;
        drawText(CX, 214, cHighlight, v.c_str());
    }

    // ---- instance name (top) -----------------------------------------
    {
        int cin = (int)snap.curInst;
        if (cin >= 0 && cin < mpdInstanceCount()) {
            setFont(s_fSmall);
            // y=30 sits between dots (y=20) and time (y=40) with
            // ~158px visible band on the round screen.
            drawText(CX, 30, cOk,
                     truncate(String(mpdInstanceName(cin)), 158).c_str());
        }
    }
    drawStatusDots(snap.wifi, snap.mpd, false);
}

// ---------------------------------------------------------------------
static void drawMenuView(const SharedState& snap) {
    spr.fillScreen(cBg);
    setFont(s_fSmall);
    drawText(CX, 34, cDim, "MENU");
    for (int i = 0; i < 6; ++i) {
        int  y   = 58 + i * 22;
        bool sel = (i == s_menuSel);
        if (sel) spr.fillRoundRect(14, y - 9, 212, 20, 8, cSel);
        drawText(CX, y, sel ? cText : cDim, s_menuItems[i]);
    }
    String foot = "hold: back " + String("(now playing)");
    drawText(CX, 218, cDim, foot.c_str());
    drawStatusDots(snap.wifi, snap.mpd, true);
}

// ---------------------------------------------------------------------
static void drawInstancesView(const SharedState& snap) {
    spr.fillScreen(cBg);
    setFont(s_fSmall);
    drawText(CX, 32, cDim, "change room");

    int cnt = mpdInstanceCount();
    if (cnt <= 0) {
        drawText(CX, 110, cDim, "none configured");
        drawStatusDots(snap.wifi, snap.mpd, true);
        return;
    }
    if (s_instSel >= cnt) s_instSel = cnt - 1;
    if (s_instSel < 0) s_instSel = 0;

    int top = windowTop(s_instSel, (uint32_t)cnt);
    for (int i = 0; i < 5; ++i) {
        int idx = top + i;
        if (idx >= cnt) break;
        int y = 58 + i * 22;
        bool sel = (idx == s_instSel);
        if (sel) spr.fillRoundRect(14, y - 9, 212, 20, 8, cSel);

        String text;
        if (idx == (int)snap.curInst) text = "[x] ";
        text += mpdInstanceName(idx);

        uint16_t col = (idx == (int)snap.curInst) ? cOk : cDim;
        if (sel) col = cText;
        drawText(CX, y, col, truncate(text, 200).c_str());
    }
    String foot = (String)(s_instSel + 1) + " / " + (String)cnt;
    drawText(CX, 218, cDim, foot.c_str());
    drawStatusDots(snap.wifi, snap.mpd, true);
}

// ---------------------------------------------------------------------
static void drawPlayMenuView(const SharedState& snap) {
    spr.fillScreen(cBg);
    setFont(s_fSmall);
    drawText(CX, 24, cDim, "PLAY");
    drawText(CX, 44, cDim, "tap to toggle");

    for (int i = 0; i < PLAY_ROWS; ++i) {
        int  y   = 68 + i * 30;
        bool sel = (i == s_playSel);
        if (sel) spr.fillRoundRect(14, y - 12, 212, 26, 8, cSel);

        uint16_t col = cDim;
        String   text;

        if (i == PLAY_RANDOM) {
            text = (snap.status.random ? "[x] " : "[ ] ") + String("Random");
            if (snap.status.random) col = cOk;
        } else if (i == PLAY_REPEAT) {
            text = (snap.status.repeat ? "[x] " : "[ ] ") + String("Repeat");
            if (snap.status.repeat) col = cOk;
        } else {
            text = "Clear playlist";
            col  = cBad;
        }
        if (sel) col = cText;
        drawText(CX, y, col, text.c_str());
    }

    String foot = "hold: back";
    drawText(CX, 208, cDim, foot.c_str());
    drawStatusDots(snap.wifi, snap.mpd, true);
}

// ---------------------------------------------------------------------
// Shared window helper for list-like rows.
static int windowTop(int sel, uint32_t count) {
    if (count == 0) return 0;
    int top = sel - 2;
    if (top < 0) top = 0;
    if ((int)count - top < 5) top = (int)count - 5;
    if (top < 0) top = 0;
    return top;
}

static void drawPlaylistView(const SharedState& snap) {
    spr.fillScreen(cBg);

    setFont(s_fSmall);
    drawText(CX, 24, cDim, "QUEUE");

    const MpdPlaylist* pl = mpdPlaylistHandle();
    if (!snap.mpd || !pl) {
        drawText(CX, 110, cDim, "no mpd");
        return;
    }

    mpdPlaylistLock();
    if (!pl->isValid()) {
        const char* msg = "loading...";
        if (snap.plErr == FE_TOOBIG)
            msg = "queue too big";
        else if (snap.plErr == FE_NET || snap.plErr == FE_MEM ||
                 snap.plErr == FE_BAD)
            msg = "load failed - retrying";
        drawText(CX, 110, cDim, msg);
        mpdPlaylistUnlock();
        return;
    }

    uint32_t pageCnt = pl->count();
    uint32_t pageSt  = pl->pageStart();
    uint32_t total   = pl->total();
    if (pageCnt == 0) {
        drawText(CX, 110, cDim, "empty queue");
        mpdPlaylistUnlock();
        return;
    }

    // s_songSel is an absolute queue position; clamp to the queue size.
    if (total > 0 && s_songSel >= (int)total) s_songSel = (int)total - 1;
    if (s_songSel < 0) s_songSel = 0;

    // If the selection isn't inside the loaded page, show a hint while the
    // task recentres (scrolling wrote gShared.plSel + task reloads).
    if (s_songSel < (int)pageSt || s_songSel >= (int)(pageSt + pageCnt)) {
        drawText(CX, 110, cDim, "loading...");
        String foot = (String)(s_songSel + 1) + " / " + (String)total;
        drawText(CX, 208, cDim, foot.c_str());
        mpdPlaylistUnlock();
        return;
    }

    // draw the window around the selection (relative to the page base)
    int selRel = s_songSel - (int)pageSt;
    int top    = windowTop(selRel, pageCnt);
    setFont(s_fSmall);
    for (int i = 0; i < 5; ++i) {
        int rel = top + i;
        if (rel >= (int)pageCnt) break;
        int idx = rel + (int)pageSt;   // absolute queue position
        int y   = 50 + i * 24;

        MpdSong e;
        if (!pl->entry(rel, e)) continue;

        String text;
        if (idx == snap.status.pos) text = "> ";
        text += (String)(idx + 1) + "  " + displayTitle(e);
        if (e.artist.length()) text += " - " + e.artist;
        text = truncate(text, 196);

        bool sel = (idx == s_songSel);
        if (sel) spr.fillRoundRect(18, y - 9, 204, 20, 8, cSel);

        uint16_t col = cDim;
        if (idx == snap.status.pos) col = cHighlight;
        if (sel) col = cText;
        drawText(CX, y, col, text.c_str());
    }

    // footer: absolute position / total queue size
    String foot = (String)(s_songSel + 1) + " / " + (String)total;
    drawText(CX, 208, cDim, foot.c_str());
    mpdPlaylistUnlock();
}

// ---------------------------------------------------------------------
// helpers to read one browse cell
static bool browseCell(uint32_t idx, MpdBrowseItem& out) {
    SharedState snap;
    snapShared(snap);
    if (!(snap.brLoaded && snap.brGen == s_reqSeq && snap.brKind == BC_ITEMS))
        return false;
    bool ok;
    mpdBrowseLock();
    ok = mpdBrowseHandle()->item(idx, out);
    mpdBrowseUnlock();
    return ok;
}

static bool browseSong(uint32_t idx, MpdSong& out) {
    SharedState snap;
    snapShared(snap);
    if (!(snap.brLoaded && snap.brGen == s_reqSeq && snap.brKind == BC_SONGS))
        return false;
    bool ok;
    mpdBrowseLock();
    ok = mpdBrowseSongsHandle()->entry(idx, out);
    mpdBrowseUnlock();
    return ok;
}

// ---------------------------------------------------------------------
static void drawBrowseView(const SharedState& snap) {
    spr.fillScreen(cBg);
    setFont(s_fSmall);

    // header: task-written title for the current frame
    String header = snap.brTitle;
    if (header.length() == 0) header = "Browse";
    drawText(CX, 22, cDim, truncate(header, 200).c_str());

    bool ready = snap.brLoaded && snap.brGen == s_reqSeq;
    if (!ready) {
        const char* msg = "loading...";
        if (snap.brErr == FE_TOOBIG)
            msg = "list too big";
        else if (snap.brErr == FE_NET)
            msg = "connection lost";
        else if (snap.brErr == FE_MEM)
            msg = "out of memory";
        else if (snap.brErr == FE_BAD)
            msg = "load failed - retrying";
        drawText(CX, 110, cDim, msg);
        return;
    }

    if (snap.brCount == 0) {
        drawText(CX, 110, cDim, "empty");
        return;
    }

    BFrame& f  = topFrame();
    int&    sel = f.sel;
    if (sel >= (int)snap.brCount) sel = (int)snap.brCount - 1;
    if (sel < 0) sel = 0;

    int     top = windowTop(sel, snap.brCount);
    uint16_t row[5];
    String  txt[5];
    bool    box[5];
    int     n = 0;

    setFont(s_fSmall);
    mpdBrowseLock();
    for (int i = 0; i < 5; ++i) {
        int idx = top + i;
        if (idx >= (int)snap.brCount) break;
        bool isSel = (idx == sel);
        if (snap.brKind == BC_ITEMS) {
            MpdBrowseItem it;
            if (!mpdBrowseHandle()->item(idx, it)) continue;
            String text;
            uint16_t col = cText;
            if (it.kind == MpdItemKind::DIRECTORY) {
                text = "[D] " + it.name;
                col  = cWarn;
            } else if (it.kind == MpdItemKind::PLAYLIST) {
                text = "[P] " + it.name;
                col  = cHighlight;
            } else if (it.kind == MpdItemKind::FILE) {
                text = displayTitle(it.song, it.name);
                if (it.song.artist.length()) text += " - " + it.song.artist;
                col = cDim;
            } else {  // TAG
                text = it.name;
            }
            txt[n]  = truncate(text, 190);
            box[n]  = isSel;
            row[n]  = isSel ? cText : col;
            n++;
        } else {  // BC_SONGS
            MpdSong s;
            if (!mpdBrowseSongsHandle()->entry(idx, s)) continue;
            String text = displayTitle(s);
            if (s.artist.length()) text += " - " + s.artist;
            txt[n] = truncate(text, 190);
            box[n] = isSel;
            row[n] = isSel ? cText : cDim;
            n++;
        }
    }
    mpdBrowseUnlock();

    for (int i = 0; i < n; ++i) {
        int y = 50 + i * 24;
        if (box[i]) spr.fillRoundRect(14, y - 9, 212, 20, 8, cSel);
        drawText(CX, y, row[i], txt[i].c_str());
    }

    bool toastOn = s_toast.length() && millis() < s_toastUntil;
    if (toastOn)
        drawText(CX, 188, s_toastCol, truncate(s_toast, 202).c_str());
    else {
        String foot = (String)(sel + 1) + " / " + (String)snap.brCount;
        drawText(CX, 208, cDim, foot.c_str());
    }
}

// ---------------------------------------------------------------------
void uiTick() {
    SharedState snap;
    snapShared(snap);

    bool marquee = false;
    if (s_mode == MODE_NOW) {
        setFont(s_fTitle);
        marquee = spr.textWidth(displayTitle(snap.song).c_str()) > TXT_W + 2;
    }

    bool toastOn = s_toast.length() && millis() < s_toastUntil;
    if (!toastOn && s_toast.length()) {
        s_toast = String();
        s_dirty = true;   // expire the bubble with one extra frame
    }

    uint32_t now = millis();
    static uint32_t lastDraw = 0;
    bool timeTick = (now - lastDraw >= 120);

    uint32_t      fsel = topSelSig();
    uint32_t      sig  = snap.statusGen + snap.songGen * 7u +
                        snap.plGen * 13u + snap.brGen * 17u +
                        (snap.wifi ? 1u : 0u) + (snap.mpd ? 2u : 0u) +
                        (snap.brLoaded ? 4u : 0u) + (toastOn ? 8u : 0u) +
                        (uint32_t)s_mode * 31u +
                        fsel * 101u + (uint32_t)s_depth * 3u +
                        (uint32_t)s_menuSel * 19u +
                        (uint32_t)s_instSel * 23u +
                        (uint32_t)snap.curInst * 29u +
                        (uint32_t)s_playSel * 31u;

    if (!timeTick && !s_dirty && sig == s_drawSig && !marquee) return;
    s_drawSig = sig;
    s_dirty   = false;
    lastDraw  = now;

    if (s_mode != s_lastMode) s_lastMode = s_mode;

    if (s_mode == MODE_NOW)
        drawNowView(snap);
    else if (s_mode == MODE_MENU)
        drawMenuView(snap);
    else if (s_mode == MODE_QUEUE)
        drawPlaylistView(snap);
    else if (s_mode == MODE_INSTS)
        drawInstancesView(snap);
    else if (s_mode == MODE_PLAYMENU)
        drawPlayMenuView(snap);
    else
        drawBrowseView(snap);
    spr.pushSprite(0, 0);
}

// ---------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------
void uiEncoder(int delta) {
    if (delta == 0) return;
    if (s_mode == MODE_NOW) {
        // accumulate to cancel ±1 encoder bounce, emit in solid steps
        s_encAcc += delta;
        if (s_encAcc >= 2 || s_encAcc <= -2) {
            post(CMD_SET_VOL, s_encAcc);
            s_encAcc = 0;
        }
    } else if (s_mode == MODE_MENU) {
        s_menuSel += delta;
        if (s_menuSel < 0) s_menuSel = 0;
        if (s_menuSel > 5) s_menuSel = 5;
        s_dirty = true;
    } else if (s_mode == MODE_INSTS) {
        s_instSel += delta;
        s_dirty = true;
    } else if (s_mode == MODE_PLAYMENU) {
        s_playSel += delta;
        if (s_playSel < 0) s_playSel = PLAY_ROWS - 1;
        if (s_playSel > PLAY_ROWS - 1) s_playSel = 0;
        s_dirty = true;
    } else {
        s_pend.armed = false;   // scrolling cancels a pending decide
        int& sel     = s_mode == MODE_QUEUE ? s_songSel : topFrame().sel;
        sel += delta;
        if (s_mode == MODE_QUEUE) {
            xSemaphoreTake(gShared.mux, portMAX_DELAY);
            gShared.plSel = s_songSel;
            xSemaphoreGive(gShared.mux);
        }
        s_dirty = true;
    }
}

// ---------------------------------------------------------------------
static void armPending() {
    SharedState snap;
    snapShared(snap);
    if (!(snap.brLoaded && snap.brGen == s_reqSeq)) return;
    if (topFrame().sel >= (int)snap.brCount) return;
    s_pend.armed = true;
    s_pend.idx   = (uint32_t)topFrame().sel;
    s_pend.depth = (uint8_t)s_depth;
    s_pend.type  = (uint8_t)topFrame().type;
    s_dirty      = true;
}

// ---------------------------------------------------------------------
void uiButtonClick() {
    switch (s_mode) {
    case MODE_NOW:
        post(CMD_PLAY_PAUSE);
        break;

    case MODE_MENU:
        switch (s_menuSel) {
        case 0:   // Queue
            s_mode = MODE_QUEUE;
            {
                SharedState snap;
                snapShared(snap);
                s_songSel = snap.status.pos >= 0 ? snap.status.pos : 0;
            }
            requestPlaylistReload();
            {
                xSemaphoreTake(gShared.mux, portMAX_DELAY);
                gShared.plSel = s_songSel;
                xSemaphoreGive(gShared.mux);
            }
            syncModes();
            s_dirty = true;
            break;
        case 1:   // Files
            enterBrowse(Fb::DIR, "");
            break;
        case 2:   // Library
            enterBrowse(Fb::ARTISTS, "");
            break;
        case 3:   // Playlists
            enterBrowse(Fb::PLISTS, "");
            break;
        case 4:   // Instances
            {
                SharedState snap0;
                snapShared(snap0);
                s_instSel = (int)snap0.curInst;
            }
            s_mode = MODE_INSTS;
            syncModes();
            s_dirty = true;
            break;
        case 5:   // Back (now playing)
            s_mode = MODE_NOW;
            syncModes();
            s_dirty = true;
            break;
        }
        break;

    case MODE_INSTS: {
        int cnt = mpdInstanceCount();
        SharedState snap1;
        snapShared(snap1);
        if (s_instSel >= 0 && s_instSel < cnt &&
            s_instSel != (int)snap1.curInst) {
            post(CMD_SET_INSTANCE, s_instSel);
            s_mode  = MODE_NOW;
            syncModes();
            s_dirty = true;
        }
        break;
    }

    case MODE_PLAYMENU:
        if (s_playSel == PLAY_RANDOM) post(CMD_SET_RANDOM, -1);
        else if (s_playSel == PLAY_REPEAT) post(CMD_SET_REPEAT, -1);
        else { post(CMD_CLEAR); showToast("playlist cleared", cWarn); }
        s_dirty = true;
        break;

    case MODE_QUEUE: {
        const MpdPlaylist* pl = mpdPlaylistHandle();
        mpdPlaylistLock();
        bool ok = pl && pl->isValid() && pl->total() > 0 &&
                  s_songSel >= 0 && s_songSel < (int)pl->total();
        mpdPlaylistUnlock();
        if (ok) {
            post(CMD_PLAY_INDEX, s_songSel);
            s_mode  = MODE_NOW;
            syncModes();
            s_dirty = true;
        }
        break;
    }

    case MODE_BROWSE: {
        const Fb t = topFrame().type;
        switch (t) {
        case Fb::DIR: {
            MpdBrowseItem it;
            if (!browseCell((uint32_t)topFrame().sel, it)) break;
            if (it.kind == MpdItemKind::DIRECTORY)
                pushFrame(Fb::DIR, it.uri);
            else
                armPending();
            break;
        }
        case Fb::ARTISTS: {
            MpdBrowseItem it;
            if (!browseCell((uint32_t)topFrame().sel, it)) break;
            pushFrame(Fb::ALBUMS, it.name);
            break;
        }
        case Fb::ALBUMS: {
            MpdBrowseItem it;
            if (!browseCell((uint32_t)topFrame().sel, it)) break;
            pushFrame(Fb::SONGS, topFrame().arg, it.name);
            break;
        }
        default:   // SONGS / PLISTS / PLS_VIEW
            armPending();
            break;
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------
// Single/double-click decision lands here once the button library has
// waited out the click window. `dbl` selects play (true) vs enqueue.
static void browseDecide(uint32_t idx, bool dbl) {
    const Fb t = topFrame().type;

    if (t == Fb::PLISTS) {
        MpdBrowseItem it;
        if (!browseCell(idx, it)) return;
        if (dbl) {
            actRequest(it.name.c_str());
            post(CMD_ACT, ACT_LOAD_PL);
            showToast(("playing: " + it.name).c_str(), cWarn);
        } else {
            pushFrame(Fb::PLS_VIEW, it.name);
        }
        s_dirty = true;
        return;
    }

    if (t == Fb::DIR) {
        MpdBrowseItem it;
        if (!browseCell(idx, it)) return;
        if (it.kind == MpdItemKind::DIRECTORY) {
            // touching/double-clicking a folder plays it
            if (!dbl) return;
            String uri = it.uri;
            actRequest(uri.c_str());
            post(CMD_ACT, (int)ACT_PLAY);
            showToast(("playing folder: " + uri).c_str(), cWarn);
            s_dirty = true;
            return;
        }
        actRequest(it.uri.c_str());
        post(CMD_ACT, dbl ? (int)ACT_PLAY : (int)ACT_ENQUEUE);
        String label = displayTitle(it.song, it.name);
        String msg   = String(dbl ? "playing: " : "queued: ") + label;
        showToast(msg.c_str(), dbl ? cWarn : cHighlight);
        s_dirty = true;
        return;
    }

    // SONGS / PLS_VIEW
    MpdSong s;
    if (!browseSong(idx, s) || s.file.length() == 0) return;
    actRequest(s.file.c_str());
    post(CMD_ACT, dbl ? (int)ACT_PLAY : (int)ACT_ENQUEUE);
    String msg = String(dbl ? "playing: " : "queued: ") + displayTitle(s);
    showToast(msg.c_str(), dbl ? cWarn : cHighlight);
    s_dirty = true;
}

// ---------------------------------------------------------------------
static void decideSingle() {
    if (s_mode != MODE_BROWSE) return;
    bool dbl;
    if (M5Dial.BtnA.wasDoubleClicked())
        dbl = true;
    else if (M5Dial.BtnA.wasSingleClicked())
        dbl = false;
    else
        return;

    if (!s_pend.armed || s_pend.depth != s_depth ||
        s_pend.type != (uint8_t)topFrame().type)
        return;
    s_pend.armed       = false;
    const uint32_t idx = s_pend.idx;
    browseDecide(idx, dbl);
}

// ---------------------------------------------------------------------
void uiButtonDecide() { decideSingle(); }

// ---------------------------------------------------------------------
void uiButtonHold() {
    if (s_mode == MODE_NOW) {
        s_mode   = MODE_MENU;
        s_menuSel = 0;
        syncModes();
    } else if (s_mode == MODE_MENU) {
        s_mode = MODE_NOW;
        syncModes();
    } else if (s_mode == MODE_QUEUE) {
        s_mode = MODE_MENU;
        syncModes();
    } else if (s_mode == MODE_INSTS) {
        s_mode = MODE_MENU;
        syncModes();
    } else if (s_mode == MODE_PLAYMENU) {
        s_mode = MODE_NOW;
        syncModes();
    } else {  // MODE_BROWSE
        popFrame();
    }
    s_pend.armed = false;
    s_dirty      = true;
}

// ---------------------------------------------------------------------
// Touch zones: small arrows beside "playing" on left/right edges.
// Tapping left half = previous, right half = next.
// Tapping the "playing" word (centre) opens the play menu (random/repeat).
// Touch must land in the arrow row (y ~ 155–200).
void uiTouchTick() {
    if (s_mode == MODE_NOW) {
        const auto& td = M5Dial.Touch.getDetail();
        if (!td.wasClicked()) return;

        int tx = td.x;
        int ty = td.y;

        // tap on the room name row → open instance selector
        if (ty >= 16 && ty <= 44) {
            s_mode = MODE_INSTS;
            syncModes();
            s_dirty = true;
            return;
        }

        // only accept taps in the arrow row band
        if (ty < 155 || ty > 200) return;

        if (tx < 100) {
            post(CMD_PREV);
            s_arrowFlash = 1;
            s_arrowUntil = millis() + 300;
            s_dirty = true;
        } else if (tx > 140) {
            post(CMD_NEXT);
            s_arrowFlash = 2;
            s_arrowUntil = millis() + 300;
            s_dirty = true;
        } else {
            s_mode  = MODE_PLAYMENU;
            s_playSel = PLAY_RANDOM;
            syncModes();
            s_dirty = true;
        }
    } else if (s_mode == MODE_BROWSE) {
        // Touching an entry plays it (acts like a double-click).
        const auto& td = M5Dial.Touch.getDetail();
        if (!td.wasClicked()) return;

        SharedState snap;
        snapShared(snap);
        if (!snap.brLoaded || snap.brCount == 0) return;

        BFrame& f = topFrame();
        int&   sel = f.sel;
        if (sel >= (int)snap.brCount) sel = (int)snap.brCount - 1;
        if (sel < 0) sel = 0;

        int top = windowTop(sel, snap.brCount);
        for (int i = 0; i < 5; ++i) {
            int idx   = top + i;
            if (idx >= (int)snap.brCount) break;
            int y = 50 + i * 24;
            if (td.y < y - 12 || td.y > y + 12) continue;
            if (idx == sel) {
                // selected entry: play now
                s_pend.armed = false;
                browseDecide((uint32_t)idx, true);
            } else {
                // other entry: select it (scrolling picks it up too)
                sel = idx;
                s_pend.armed = false;
                s_dirty      = true;
            }
            s_dirty = true;
            break;
        }
    } else if (s_mode == MODE_PLAYMENU) {
        // tapping a row toggles it; tapping elsewhere does nothing
        const auto& td = M5Dial.Touch.getDetail();
        if (!td.wasClicked()) return;
        int ty = td.y;
        for (int i = 0; i < PLAY_ROWS; ++i) {
            int y = 68 + i * 30;
            if (ty >= y - 14 && ty <= y + 14) {
                if (i == PLAY_RANDOM) post(CMD_SET_RANDOM, -1);
                else if (i == PLAY_REPEAT) post(CMD_SET_REPEAT, -1);
                else { post(CMD_CLEAR); showToast("playlist cleared", cWarn); }
                s_dirty = true;
                break;
            }
        }
    }
}