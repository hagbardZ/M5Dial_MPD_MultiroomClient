#pragma once

#include "MpdClient.h"
#include "Playlist.h"

// ---------------------------------------------------------------------
// Browsable items: everything the UI needs to render one row of a
// directory / tag / playlist listing.
enum class MpdItemKind : uint8_t {
    DIRECTORY,
    FILE,
    PLAYLIST,
    TAG,          // an artist or album name
};

struct MpdBrowseItem {
    MpdItemKind   kind;
    String        name;   // display name (basename, tag value, playlist name)
    String        uri;    // full path / value used for commands
    MpdSong       song;   // parsed tags for FILE items (may be invalid())
};

// ---------------------------------------------------------------------
// PSRAM-friendly listing used by the browser:
//   - "lsinfo" directory listings (dirs, files, playlists)
//   - "list <tag>" values (artists / albums)
//   - "listplaylists" (stored playlist names)
//
// The raw response is kept as one contiguous buffer (SPIRAM preferred)
// together with compact {kind, offsets, block} entries, so large listings
// fit in memory.  MpdPlaylist::parseBlock() is reused for FILE tag
// extraction.
class MpdBrowseList {
public:
    MpdBrowseList()  = default;
    ~MpdBrowseList() { clear(); }

    MpdBrowseList(const MpdBrowseList&)            = delete;
    MpdBrowseList& operator=(const MpdBrowseList&) = delete;

    // "lsinfo [dir]" - list the given directory ("" = music root).
    bool loadDir(MpdClient& c, const String& dir);
    // a "list <cmd>" response of "<key>: value" lines (artists/albums/etc).
    bool loadTags(MpdClient& c, const String& cmd, const char* key);
    // "listplaylists".
    bool loadPlaylists(MpdClient& c);

    void     clear();
    bool     isValid() const { return _data != nullptr; }
    uint32_t count() const { return _count; }
    bool     item(uint32_t index, MpdBrowseItem& out) const;

private:
    // Full entry used by loadDir (needs blockStart/blockLen for song parsing)
    struct Ent {
        uint8_t  kind;
        uint32_t nameOff, nameLen;
        uint32_t uriOff, uriLen;
        uint32_t blockStart, blockLen;
    };

    bool _append(MpdItemKind k, const char* bufBase, const char* val,
                 size_t vlen, bool takeBase, uint32_t blockStart);

    // Full mode (loadDir)
    Ent*   _ent   = nullptr;
    size_t _cap   = 0;

    // Compact mode (loadTags / loadPlaylists) — TagEnt {off, len} per entry
    void*    _starts = nullptr;

    // Shared
    char*    _data  = nullptr;
    size_t   _len   = 0;
    uint32_t _count = 0;
    uint8_t  _compactKind = 0; // TAG or PLAYLIST when _starts is active
};