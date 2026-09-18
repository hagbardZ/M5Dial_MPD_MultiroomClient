#pragma once

#include "MpdClient.h"

// ---------------------------------------------------------------------
// PSRAM-friendly representation of the MPD song queue.
//
// The raw "playlistinfo" response is kept as one contiguous buffer
// (SPIRAM preferred) together with the byte offset of every song block,
// so large queues can be held with minimal heap usage.
class MpdPlaylist {
public:
    MpdPlaylist()  = default;
    ~MpdPlaylist() { clear(); }

    MpdPlaylist(const MpdPlaylist&)            = delete;
    MpdPlaylist& operator=(const MpdPlaylist&) = delete;

    bool     reload(MpdClient& client);
    // Issue "playlistinfo START:START+count" and keep only that window.
    // total is the full queue length (from MPD status) used for clamping.
    bool     reloadPage(MpdClient& client, uint32_t start, uint32_t count,
                        uint32_t total);
    // Like reload(), but issues an arbitrary MPD command whose response is
    // a sequence of "song" blocks (e.g. "find ...", "listplaylistinfo ...").
    bool     reloadCmd(MpdClient& client, const String& cmd);
    void     clear();
    bool     isValid() const { return _data != nullptr; }
    uint32_t count() const { return _count; }
    // Absolute queue index of the first loaded entry (page window base).
    uint32_t pageStart() const { return _pageStart; }
    // Full queue length known at load time.
    uint32_t total() const { return _total; }
    bool     entry(uint32_t index, MpdSong& out) const;

    // "file: xyz" of a given song (without parsing everything)
    bool     fileOf(uint32_t index, String& file) const;

    // Parse a "song" block (file:/Artist:/Album:/Title:/Name:/Pos:/Id:).
    static bool parseBlock(const char* start, const char* end, MpdSong& out);

private:
    char*     _data    = nullptr;
    size_t    _len     = 0;
    uint32_t* _starts  = nullptr;
    uint32_t  _count   = 0;
    uint32_t  _pageStart = 0;
    uint32_t  _total     = 0;
};