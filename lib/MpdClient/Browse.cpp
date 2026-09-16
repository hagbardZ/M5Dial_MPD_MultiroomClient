#include "Browse.h"

#include <string.h>

#include "esp_heap_caps.h"

// ---------------------------------------------------------------------
void MpdBrowseList::clear() {
    if (_ent) free(_ent);
    if (_data) free(_data);
    if (_starts) free(_starts);
    _ent      = nullptr;
    _starts   = nullptr;
    _cap      = 0;
    _data     = nullptr;
    _len      = 0;
    _count    = 0;
    _compactKind = 0;
}

// ---------------------------------------------------------------------
bool MpdBrowseList::_append(MpdItemKind k, const char* base,
                            const char* vstart, size_t vlen,
                            bool takeBase, uint32_t blockStart) {
    if (_count == _cap) {
        size_t nc = _cap ? _cap * 2 : 32;
        Ent*   ne = (Ent*)heap_caps_realloc(_ent, nc * sizeof(Ent),
                                            MALLOC_CAP_SPIRAM);
        if (!ne) ne = (Ent*)realloc(_ent, nc * sizeof(Ent));
        if (!ne) return false;
        _ent = ne;
        _cap = nc;
    }

    Ent& e = _ent[_count];
    e.kind   = (uint8_t)k;
    e.uriOff = (uint32_t)(vstart - base);
    e.uriLen = (uint32_t)vlen;

    if (takeBase) {  // shorten the display name to the last path segment
        uint32_t off = 0;
        for (uint32_t i = 0; i < vlen; ++i)
            if (vstart[i] == '/') off = i + 1;
        e.nameOff = e.uriOff + off;
        e.nameLen = vlen - off;
        if (!e.nameLen) {  // "a/b/" style -> keep the whole value
            e.nameOff = e.uriOff;
            e.nameLen = vlen;
        }
    } else {
        e.nameOff = e.uriOff;
        e.nameLen = vlen;
    }
    e.blockStart = blockStart;
    e.blockLen   = 0;
    _count++;
    return true;
}

// ---------------------------------------------------------------------
// lsinfo response groups files with their tags into contiguous blocks:
//   directory: Music/Artist
//   file: Music/Artist/Album/track.flac
//   AlbumArtist: ...
//   Album: ...
//   Title: ...
//   playlist: name.m3u
bool MpdBrowseList::loadDir(MpdClient& c, const String& dir) {
    clear();
    char*  buf = nullptr;
    size_t len = 0;
    String cmd = "lsinfo";
    if (dir.length()) {
        // Quote the path: names like "1 Harry Potter - ..." contain spaces.
        cmd += " \"";
        cmd += dir;
        cmd += "\"";
    }
    if (!c.fetchLineList(cmd, &buf, &len)) return false;

    const char* p   = buf;
    const char* end = buf + len;
    int lastFile    = -1;  // FILE block lengths are finalized on the next entry

    while (p < end) {
        const char* nl   = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* le   = nl ? nl : end;
        size_t      llen = (size_t)(le - p);

        MpdItemKind k   = (MpdItemKind)0xFF;
        const char* val = nullptr;
        size_t      vlen = 0;
        if (llen >= 10 && memcmp(p, "directory:", 10) == 0) {
            k = MpdItemKind::DIRECTORY;
            val = p + 10;
        } else if (llen >= 5 && memcmp(p, "file:", 5) == 0) {
            k = MpdItemKind::FILE;
            val = p + 5;
        } else if (llen >= 9 && memcmp(p, "playlist:", 9) == 0) {
            k = MpdItemKind::PLAYLIST;
            val = p + 9;
        }

        if (k != (MpdItemKind)0xFF) {
            vlen = llen - (size_t)(val - p);
            while (vlen && (*val == ' ' || *val == '\t')) { ++val; --vlen; }
            while (vlen && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' ||
                            val[vlen - 1] == '\r'))
                --vlen;
            if (vlen) {
                if (!_append(k, buf, val, vlen,
                             k != MpdItemKind::PLAYLIST,
                             (uint32_t)(p - buf))) {
                    free(buf);
                    return false;
                }
                if (lastFile >= 0)
                    _ent[lastFile].blockLen =
                        (uint32_t)(p - buf) - _ent[lastFile].blockStart;
                lastFile = (k == MpdItemKind::FILE) ? (int)(_count - 1) : -1;
            }
        }
        p = le + 1;
        if (!nl) break;
    }
    if (lastFile >= 0)
        _ent[lastFile].blockLen = (uint32_t)len - _ent[lastFile].blockStart;

    _data = buf;
    _len  = len;
    return true;
}

// ---------------------------------------------------------------------
// "list <tag>" responses are single-line "<key>: <value>" pairs.
// Uses a compact starts[] array (8 bytes/entry) instead of Ent (28 bytes)
// so large listings (2000+ artists) fit in internal RAM.
bool MpdBrowseList::loadTags(MpdClient& c, const String& cmd, const char* key) {
    clear();
    char*  buf = nullptr;
    size_t len = 0;
    if (!c.fetchLineList(cmd, &buf, &len)) return false;

    size_t      klen = strlen(key);
    const char* p    = buf;
    const char* end  = buf + len;

    // First pass: count matching lines to allocate starts array.
    uint32_t n = 0;
    {
        const char* q = p;
        while (q < end) {
            const char* nl = (const char*)memchr(q, '\n', (size_t)(end - q));
            size_t      ll = nl ? (size_t)(nl - q) : (size_t)(end - q);
            if (ll > klen + 1 && memcmp(q, key, klen) == 0 && q[klen] == ':')
                ++n;
            if (!nl) break;
            q = nl + 1;
        }
    }

    if (n == 0) {
        _data = buf;
        _len  = len;
        return true;   // empty but valid
    }

    // Each entry: {off, len} — offset and length of the VALUE in the buffer.
    struct TagEnt { uint32_t off; uint32_t len; };
    TagEnt* te = (TagEnt*)heap_caps_malloc(n * sizeof(TagEnt), MALLOC_CAP_SPIRAM);
    if (!te) te = (TagEnt*)malloc(n * sizeof(TagEnt));
    if (!te) { free(buf); return false; }

    uint32_t idx = 0;
    while (p < end && idx < n) {
        const char* nl   = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* le   = nl ? nl : end;
        size_t      llen = (size_t)(le - p);

        if (llen > klen + 1 && memcmp(p, key, klen) == 0 && p[klen] == ':') {
            const char* val = p + klen + 1;
            size_t      vlen = llen - klen - 1;
            while (vlen && (*val == ' ' || *val == '\t')) { ++val; --vlen; }
            while (vlen && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' ||
                            val[vlen - 1] == '\r'))
                --vlen;
            te[idx].off = (uint32_t)(val - buf);
            te[idx].len = (uint32_t)vlen;
            ++idx;
        }
        p = le + 1;
        if (!nl) break;
    }

    _data        = buf;
    _len         = len;
    _count       = n;
    _starts      = te;
    _compactKind = (uint8_t)MpdItemKind::TAG;
    return true;
}

// ---------------------------------------------------------------------
bool MpdBrowseList::loadPlaylists(MpdClient& c) {
    clear();
    char*  buf = nullptr;
    size_t len = 0;
    if (!c.fetchLineList("listplaylists", &buf, &len)) return false;

    const char* p   = buf;
    const char* end = buf + len;

    // Count playlist lines first.
    uint32_t n = 0;
    {
        const char* q = p;
        while (q < end) {
            const char* nl = (const char*)memchr(q, '\n', (size_t)(end - q));
            size_t      ll = nl ? (size_t)(nl - q) : (size_t)(end - q);
            if (ll >= 9 && memcmp(q, "playlist:", 9) == 0) ++n;
            if (!nl) break;
            q = nl + 1;
        }
    }

    if (n == 0) {
        _data = buf;
        _len  = len;
        return true;
    }

    struct TagEnt { uint32_t off; uint32_t len; };
    TagEnt* te = (TagEnt*)heap_caps_malloc(n * sizeof(TagEnt), MALLOC_CAP_SPIRAM);
    if (!te) te = (TagEnt*)malloc(n * sizeof(TagEnt));
    if (!te) { free(buf); return false; }

    uint32_t idx = 0;
    while (p < end && idx < n) {
        const char* nl   = (const char*)memchr(p, '\n', (size_t)(end - p));
        const char* le   = nl ? nl : end;
        size_t      llen = (size_t)(le - p);

        if (llen >= 9 && memcmp(p, "playlist:", 9) == 0) {
            const char* val = p + 9;
            size_t      vlen = llen - 9;
            while (vlen && (*val == ' ' || *val == '\t')) { ++val; --vlen; }
            while (vlen && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' ||
                            val[vlen - 1] == '\r'))
                --vlen;
            te[idx].off = (uint32_t)(val - buf);
            te[idx].len = (uint32_t)vlen;
            ++idx;
        }
        p = le + 1;
        if (!nl) break;
    }

    _data        = buf;
    _len         = len;
    _count       = n;
    _starts      = te;
    _compactKind = (uint8_t)MpdItemKind::PLAYLIST;
    return true;
}

// ---------------------------------------------------------------------
bool MpdBrowseList::item(uint32_t index, MpdBrowseItem& out) const {
    if (index >= _count || !_data) return false;
    out = MpdBrowseItem();

    // Compact mode (tags / playlists) — entries stored in starts[] array
    if (_starts && !_ent) {
        struct TagEnt { uint32_t off; uint32_t len; };
        const TagEnt* te = (const TagEnt*)_starts;
        out.kind = (MpdItemKind)_compactKind;
        out.name = String(_data + te[index].off, te[index].len);
        out.uri  = out.name;  // tag value IS the URI
        return true;
    }

    // Full mode (loadDir) — entries in _ent array
    if (!_ent) return false;
    const Ent& e = _ent[index];

    out.kind = (MpdItemKind)e.kind;
    out.name = String(_data + e.nameOff, e.nameLen);
    out.uri  = String(_data + e.uriOff, e.uriLen);

    if (e.kind == (uint8_t)MpdItemKind::FILE && e.blockLen > 0) {
        const char* s = _data + e.blockStart;
        if (e.blockStart + e.blockLen <= _len)
            MpdPlaylist::parseBlock(s, s + e.blockLen, out.song);
        if (out.song.title.length()) out.name = out.song.title;
    }
    return true;
}