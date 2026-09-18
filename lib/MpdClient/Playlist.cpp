#include "Playlist.h"

#include "esp_heap_caps.h"

// ---------------------------------------------------------------------
void MpdPlaylist::clear() {
    if (_data) free(_data);
    if (_starts) free(_starts);
    _data   = nullptr;
    _len    = 0;
    _starts = nullptr;
    _count  = 0;
    _pageStart = 0;
    _total     = 0;
}

// ---------------------------------------------------------------------
bool MpdPlaylist::reload(MpdClient& client) {
    return reloadCmd(client, "playlistinfo");
}

// ---------------------------------------------------------------------
bool MpdPlaylist::reloadPage(MpdClient& client, uint32_t start,
                             uint32_t count, uint32_t total) {
    if (!reloadCmd(client, "playlistinfo " + String(start) + ":" +
                                String(start + count)))
        return false;
    _pageStart = start;
    _total     = total;
    return true;
}

// ---------------------------------------------------------------------
bool MpdPlaylist::reloadCmd(MpdClient& client, const String& cmd) {
    char*  buf = nullptr;
    size_t len = 0;
    if (!client.fetchLineList(cmd, &buf, &len)) return false;

    // count "file:" lines and record the offset of each song block
    uint32_t cap    = 128, n = 0;
    uint32_t* starts = (uint32_t*)heap_caps_malloc(cap * sizeof(uint32_t),
                                                   MALLOC_CAP_SPIRAM);
    if (!starts) starts = (uint32_t*)malloc(cap * sizeof(uint32_t));
    if (!starts) {
        free(buf);
        return false;
    }

    const char* p   = buf;
    const char* end = buf + len;
    while (p < end) {
        const char* nl   = (const char*)memchr(p, '\n', (size_t)(end - p));
        size_t      llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (llen >= 5 && memcmp(p, "file:", 5) == 0) {
            if (n == cap) {
                cap *= 2;
                uint32_t* ns =
                    (uint32_t*)heap_caps_realloc(starts, cap * sizeof(uint32_t),
                                                 MALLOC_CAP_SPIRAM);
                if (!ns) ns = (uint32_t*)realloc(starts, cap * sizeof(uint32_t));
                if (!ns) {
                    free(starts);
                    free(buf);
                    return false;
                }
                starts = ns;
            }
            starts[n++] = (uint32_t)(p - buf);
        }
        if (!nl) break;
        p = nl + 1;
    }

    clear();
    _data   = buf;
    _len    = len;
    _starts = starts;
    _count  = n;
    return true;
}

// ---------------------------------------------------------------------
bool MpdPlaylist::parseBlock(const char* p, const char* end, MpdSong& out) {
    out = MpdSong();
    bool have = false;
    while (p < end) {
        const char* nl   = (const char*)memchr(p, '\n', (size_t)(end - p));
        size_t      llen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        const char* colon = (const char*)memchr(p, ':', llen);
        if (colon) {
            String key(p, (size_t)(colon - p));
            key.trim();
            const char* vstart = colon + 1;
            while (vstart < p + llen && (*vstart == ' ' || *vstart == '\t'))
                vstart++;
            String val(vstart, (size_t)((p + llen) - vstart));
            if (key == "file") {
                out.file = val;
                have = true;
            } else if (key == "Artist") {
                out.artist = val;
            } else if (key == "Album") {
                out.album = val;
            } else if (key == "Title") {
                out.title = val;
            } else if (key == "Name") {
                out.name = val;
            } else if (key == "Pos") {
                out.pos = val.toInt();
            } else if (key == "Id") {
                out.id = val.toInt();
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return have;
}

// ---------------------------------------------------------------------
bool MpdPlaylist::entry(uint32_t index, MpdSong& out) const {
    if (index >= _count || !_data || !_starts) return false;
    const char* start = _data + _starts[index];
    const char* end   = (index + 1 < _count) ? _data + _starts[index + 1]
                                             : _data + _len;
    return parseBlock(start, end, out);
}

// ---------------------------------------------------------------------
bool MpdPlaylist::fileOf(uint32_t index, String& file) const {
    if (index >= _count || !_data || !_starts) return false;
    const char* start = _data + _starts[index];
    const char* end   = (index + 1 < _count) ? _data + _starts[index + 1]
                                             : _data + _len;
    if ((size_t)(end - start) < 6 || memcmp(start, "file:", 5) != 0)
        return false;
    size_t llen = (size_t)(end - start);
    const char* nl = (const char*)memchr(start, '\n', llen);
    size_t eol = nl ? (size_t)(nl - start) : llen;
    String val(start + 5, eol - 5);
    val.trim();
    file = val;
    return file.length() > 0;
}