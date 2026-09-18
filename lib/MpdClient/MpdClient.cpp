#include "MpdClient.h"

#include "esp_heap_caps.h"

// ---------------------------------------------------------------------
void MpdClient::begin(const char* host, uint16_t port, const char* password) {
    _host = host;
    _port = port;
    _pass = password ? password : "";
    disconnect();
}

// ---------------------------------------------------------------------
void MpdClient::end() { disconnect(); }

// ---------------------------------------------------------------------
void MpdClient::disconnect() {
    _connected = false;
    _sock.stop();
}

// ---------------------------------------------------------------------
// MPD 0.23.5 requires quoting for any command argument containing spaces
// or special chars.  Wrap the argument in double-quotes; internal quotes
// are backslash-escaped.
static String _q(const String& s) {
    String r = "\"";
    for (unsigned i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    r += '"';
    return r;
}

// ---------------------------------------------------------------------
bool MpdClient::connect() {
    disconnect();
    if (!_sock.connect(_host.c_str(), _port)) return false;
    _sock.setTimeout(1);  // blocking reads are < 1s, we poll manually anyway

    String greet;
    if (!_readLine(greet, 4000) || !greet.startsWith("OK MPD")) {
        _sock.stop();
        return false;
    }
    if (_pass.length() > 0) {
        if (!sendCommand("password " + _pass)) {
            _sock.stop();
            return false;
        }
    }
    _connected = true;
    return true;
}

// ---------------------------------------------------------------------
bool MpdClient::_readLine(String& line, uint32_t timeoutMs) {
    line = String();
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (!_sock.connected()) return false;
        int c = _sock.read();
        if (c >= 0) {
            if (c == '\n') {
                if (line.length() && line[line.length() - 1] == '\r')
                    line.remove(line.length() - 1);
                return true;
            }
            line += (char)c;
            if (line.length() > 8192) return false;  // protocol desync, bail out
        } else {
            delay(1);
        }
    }
    return false;
}

// ---------------------------------------------------------------------
bool MpdClient::_expectOk(uint32_t timeoutMs) {
    String line;
    while (_readLine(line, timeoutMs)) {
        if (line == "OK") return true;
        if (line.startsWith("ACK")) {
            _ack = line;
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
bool MpdClient::sendCommand(const String& cmd) {
    if (!_sock.connected()) return false;
    if (_idling) endIdle();
    _sock.print(cmd);
    _sock.print("\n");
    _ack = String();
    return _expectOk(4000);
}

// ---------------------------------------------------------------------
bool MpdClient::startIdle() {
    if (!_sock.connected()) return false;
    // NB: "volume" is NOT an idle subsystem in MPD 0.23 - the whole command
    // would be rejected with an ACK and idle would never engage.
    _sock.print(
        "idle database update stored_playlist playlist player mixer options "
        "output\n");
    _idling  = true;
    _pbs     = String();
    _pending = String();
    return true;
}

// ---------------------------------------------------------------------
// Non-blocking.  Buffers subsystem lines internally and only reports the
// change list once the idle response's final "OK" has arrived (at which
// point we have left idle mode).
String MpdClient::pollIdle() {
    if (!_idling) return String();
    if (!_sock.connected()) {
        _idling  = false;
        _pending = String();
        return String();
    }

    uint32_t budget = 8192;  // bytes scanned per poll, prevents tight spins
    while (budget-- && _sock.available()) {
        int c = _sock.read();
        if (c < 0) continue;
        if (c == '\n') {
            String line = _pbs;
            _pbs = String();
            if (line.length() && line[line.length() - 1] == '\r')
                line.remove(line.length() - 1);
            if (line == "OK") {
                _idling = false;
                String done = _pending;
                _pending = String();
                return done;
            }
            if (line.length()) {
                if (_pending.length()) _pending += ",";
                _pending += line;
            }
        } else {
            _pbs += (char)c;
        }
    }
    return String();  // partial data or nothing yet
}

// ---------------------------------------------------------------------
// drain whatever the server produced after 'noidle' (subsystem names + OK)
void MpdClient::_flushIdleResponse() {
    String line;
    for (int i = 0; i < 8; ++i) {
        if (!_readLine(line, 1500)) break;
        if (line == "OK" || line.length() == 0) break;
    }
}

// ---------------------------------------------------------------------
void MpdClient::endIdle() {
    if (!_idling) return;
    if (_sock.connected()) {
        _sock.print("noidle\n");
        _flushIdleResponse();
    }
    _idling  = false;
    _pbs     = String();
    _pending = String();
}

// ---------------------------------------------------------------------
bool MpdClient::status(MpdStatus& out) {
    if (!_sock.connected()) return false;
    if (_idling) endIdle();
    _sock.print("status\n");
    out    = MpdStatus();
    String line;
    bool   ok = false;
    while (_readLine(line, 4000)) {
        if (line == "OK") {
            ok = true;
            break;
        }
        if (line.startsWith("ACK")) return false;
        int i = line.indexOf(':');
        if (i < 0) continue;
        String k = line.substring(0, i);
        k.trim();
        String v = line.substring(i + 1);
        v.trim();
        if (k == "volume") {
            out.volume = v.toInt();
        } else if (k == "state") {
            out.playing = (v == "play");
            out.paused  = (v == "pause");
            out.stopped = (v == "stop");
        } else if (k == "elapsed") {
            out.elapsed = (int)v.toFloat();
        } else if (k == "duration") {
            out.duration = (int)v.toFloat();
        } else if (k == "song") {
            out.pos = v.toInt();
        } else if (k == "songid") {
            out.songId = (uint32_t)v.toInt();
        } else if (k == "playlistlength") {
            out.plength = v.toInt();
        } else if (k == "repeat") {
            out.repeat = (v == "1");
        } else if (k == "random") {
            out.random = (v == "1");
        }
    }
    if (!ok) return false;
    out.valid = true;
    return true;
}

// ---------------------------------------------------------------------
bool MpdClient::currentSong(MpdSong& out) {
    if (!_sock.connected()) return false;
    if (_idling) endIdle();
    _sock.print("currentsong\n");
    out    = MpdSong();
    String line;
    bool   ok = false;
    while (_readLine(line, 4000)) {
        if (line == "OK") {
            ok = true;
            break;
        }
        if (line.startsWith("ACK")) return false;
        int i = line.indexOf(':');
        if (i < 0) continue;
        String k = line.substring(0, i);
        k.trim();
        String v = line.substring(i + 1);
        v.trim();
        if (k == "file") {
            out.file = v;
        } else if (k == "Artist") {
            out.artist = v;
        } else if (k == "Album") {
            out.album = v;
        } else if (k == "Title") {
            out.title = v;
        } else if (k == "Name") {
            out.name = v;
        } else if (k == "Pos") {
            out.pos = v.toInt();
        } else if (k == "Id") {
            out.id = v.toInt();
        }
    }
    return ok;
}

// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// Safe upper bound for the buffered multi-line responses.  The Dial has
// no working PSRAM; this keeps the whole response inside the internal RAM
// heap and prevents an OOM when the queue is huge.
static constexpr size_t kPlaylistCap = 120 * 1024;

// ---------------------------------------------------------------------
// Dynamic cap: use PSRAM if allocation actually works, else protect
// internal RAM.  (heap_caps_get_total_size reports the *configured* size
// even when the chip's PSRAM is dead, so verify with a real allocation.)
static size_t s_cap = 0;
static void _initCap() {
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram > 256 * 1024) {
        void* test = heap_caps_malloc(256, MALLOC_CAP_SPIRAM);
        if (test) {
            heap_caps_free(test);
            s_cap = psram / 2;
        }
    }
    if (!s_cap) s_cap = kPlaylistCap;
    Serial.printf("[mpd] psram_total=%u cap=%u\n", (unsigned)psram,
                  (unsigned)s_cap);
}

bool MpdClient::fetchLineList(const String& cmd, char** buf, size_t* len) {
    *buf       = nullptr;
    *len       = 0;
    _fetchErr  = FE_OK;
    if (!_sock.connected()) {
        _fetchErr = FE_NET;
        return false;
    }
    if (_idling) endIdle();

    _sock.print(cmd);
    _sock.print("\n");

    size_t cap = 8192, used = 0;
    char*  d = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!d) d = (char*)malloc(cap);
    if (!d) {
        _fetchErr = FE_MEM;
        disconnect();          // command already sent; socket has stale data
        return false;
    }

    if (!s_cap) _initCap();

    String line;
    bool   ok   = false;
    FetchErr fail = FE_NET;
    while (_readLine(line, 5000)) {
        if (line == "OK") {
            ok   = true;
            break;
        }
        if (line.startsWith("ACK")) {
            fail = FE_BAD;
            break;
        }
        size_t need = used + line.length() + 2;
        if (need > cap) {
            if (need > s_cap) {
                fail = FE_TOOBIG;
                break;
            }
            // Grow to exactly what's needed (16-byte aligned) instead of
            // doubling: doubling makes a 64KB realloc peak hit ~128KB,
            // which OOMs against the Dial's ~115KB internal heap.
            size_t nc = ((need + 15) / 16) * 16;
            char* nd = (char*)heap_caps_realloc(d, nc, MALLOC_CAP_SPIRAM);
            if (!nd) nd = (char*)realloc(d, nc);
            if (!nd) {
                fail = FE_MEM;
                break;
            }
            d   = nd;
            cap = nc;
        }
        memcpy(d + used, line.c_str(), line.length());
        used += line.length();
        d[used++] = '\n';
    }
    if (!ok) {
        _fetchErr = fail;
        free(d);
        disconnect();          // stale data on socket → force clean reconnect
        return false;
    }
    d[used] = 0;
    *buf = d;
    *len = used;
    return true;
}

// ---------------------------------------------------------------------
bool MpdClient::setVolume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return sendCommand("setvol " + String(vol));
}

// ---------------------------------------------------------------------
bool MpdClient::togglePlay() {
    MpdStatus s;
    if (!status(s)) return false;
    if (s.playing) return sendCommand("pause 1");
    return sendCommand("pause 0");
}

// ---------------------------------------------------------------------
bool MpdClient::next()   { return sendCommand("next"); }
bool MpdClient::prev()   { return sendCommand("previous"); }
bool MpdClient::stop()   { return sendCommand("stop"); }
bool MpdClient::playPos(int pos) { return sendCommand("play " + String(pos)); }
bool MpdClient::setRandom(bool on) { return sendCommand(on ? "random 1" : "random 0"); }
bool MpdClient::setRepeat(bool on) { return sendCommand(on ? "repeat 1" : "repeat 0"); }

// ---------------------------------------------------------------------
// "add" takes the rest of the line as its single argument, so URIs/filenames
// containing spaces pass through verbatim.
bool MpdClient::addUri(const String& uri) {
    return sendCommand("add " + _q(uri));
}

bool MpdClient::loadPlaylist(const String& name) {
    return sendCommand("load " + _q(name));
}