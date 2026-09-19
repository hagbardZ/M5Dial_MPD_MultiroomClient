#pragma once

#include <Arduino.h>
#include <WiFi.h>

// ---------------------------------------------------------------------
// Minimum data carried by MPD "status"
struct MpdStatus {
    bool     valid    = false;
    int      volume   = -1;
    bool     playing  = false;
    bool     paused   = false;
    bool     stopped  = true;
    bool     repeat   = false;
    bool     random   = false;
    bool     single   = false;
    int      elapsed  = 0;    // seconds
    int      duration = 0;    // seconds
    int      pos      = -1;   // queue position of current song
    int      plength  = -1;   // songs in the queue
    uint32_t songId   = 0;
};

// ---------------------------------------------------------------------
// Song metadata (tags)
struct MpdSong {
    String file;
    String artist;
    String album;
    String title;
    // Stream "Name" tag: the EXTINF title of a stored-playlist entry
    // (listplaylistinfo) or the stream's icy name in the queue.
    String name;
    int    pos = -1;
    int    id  = -1;
    bool   valid() const { return file.length() > 0 || title.length() > 0; }
};

// ---------------------------------------------------------------------
// Why the last fetchPlaylist() call failed (0 = ok).
enum FetchErr : int8_t {
    FE_OK     = 0,
    FE_NET    = 1,   // socket/line read failed or timed out
    FE_MEM    = 2,   // could not allocate buffer
    FE_TOOBIG = 3,   // queue response exceeded the RAM safe cap
    FE_BAD    = 4,   // server rejected the query (ACK/EOF)
};

// ---------------------------------------------------------------------
// Minimal MPD protocol client (blocking, intended for a background task).
// Speaks plain-text MPD over a WiFi socket on port 6600.
class MpdClient {
public:
    void   begin(const char* host, uint16_t port, const char* password);
    bool   connect();
    void   disconnect();
    void   end();
    bool   isConnected() { return _connected && _sock.connected(); }

    // Low level -------------------------------------------------------
    bool   sendCommand(const String& cmd);
    // Asynchronuous "idle" handling.  startIdle() puts the server into
    // idle mode; pollIdle() is non-blocking and returns a comma separated
    // list of changed subsystems the moment an event arrives (leaving idle
    // mode).  endIdle() cancels idle mode (used before issuing commands or
    // on timeout cycles).
    bool   startIdle();
    String pollIdle();
    void   endIdle();
    bool   idleActive() const { return _idling; }

    // Info ------------------------------------------------------------
    bool   status(MpdStatus& out);
    bool   currentSong(MpdSong& out);
    // Raw multi-line response into a growable buffer (PSRAM preferred).
    // Caller owns *buf and must free with heap_caps_free().
    // If `keep` is non-null, only lines starting with one of the `nKeep`
    // prefixes are stored in the buffer - all other lines are still read
    // from the socket (protocol stays in sync) but discarded.  This keeps
    // bulky responses small (e.g. lsinfo drops per-file tag blocks) so they
    // fit the internal-RAM heap on parts without usable PSRAM.
    bool fetchLineList(const String& cmd, char** buf, size_t* len,
                       const char* const* keep = nullptr,
                       size_t nKeep = 0);
    // Reason the last fetchLineList() call failed (FE_OK on success).
    FetchErr lastFetchErr() const { return _fetchErr; }

    // Controls --------------------------------------------------------
    bool   setVolume(int vol);       // clamped 0..100
    bool   togglePlay();
    bool   next();
    bool   prev();
    bool   stop();
    bool   playPos(int pos);
    bool   addUri(const String& uri);      // "add <uri>"
    bool   loadPlaylist(const String& name);  // "load <name>"
    bool   updateDb(const String& dir);    // "update <dir>" ("" = whole DB)
    bool   setRandom(bool on);
    bool   setRepeat(bool on);
    bool   setSingle(bool on);

    const String& ack() const { return _ack; }

private:
    bool   _readLine(String& line, uint32_t timeoutMs);
    bool   _expectOk(uint32_t timeoutMs);
    void   _flushIdleResponse();

    WiFiClient _sock;
    String     _host;
    uint16_t   _port     = 6600;
    String     _pass;
    bool       _connected = false;
    bool       _idling    = false;
    String     _pbs;      // partial idle line buffer
    String     _pending;  // completed subsystem lines, awaiting the final OK
    String     _ack;
    FetchErr   _fetchErr  = FE_OK;
};