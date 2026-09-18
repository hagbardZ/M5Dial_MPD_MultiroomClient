#pragma once

#include <Arduino.h>

// Starts the background task that keeps Wi-Fi + MPD connected and
// reacts to idle events.  Writes into gShared, consumes gCmdQueue.
void startMpdTask();

// Freeze / un-freeze the network task for standby (light sleep).  When
// entering standby, call mpdSetStandby(true) BEFORE disabling Wi-Fi.
void mpdSetStandby(bool on);

// ---- MPD instances (rooms) ------------------------------------------
// One MPD per room, selectable from the UI menu.  The table comes from
// config.h (MPD_INSTANCES).  The last choice is remembered across reboots
// (NVS).  Switch by posting CMD_SET_INSTANCE with an index.
struct MpdInstance {
    const char* name;
    uint16_t    port;
};
int  mpdInstanceCount();            // # of configured instances
const char* mpdInstanceName(int i); // "" for out of range
int  mpdCurrentInstance();          // currently active index

// Ask the MPD task to (re)load the song queue into the playlist buffer.
void requestPlaylistReload();

// Tell the MPD task whether the UI is showing the queue view, so it
// keeps retrying a failed queue load while that view is open.
void setPlaylistMode(bool on);

// Tell the MPD task whether the UI is inside the browser, so it keeps
// retrying a failed browse load while the browser is open.
void setBrowseMode(bool on);

class MpdBrowseList;
struct MpdPlaylist;

// The playlist is owned by the MPD task.  Wrap any reads with
// mpdPlaylistLock()/mpdPlaylistUnlock() around the whole UI pass
// that touches it; reloads take the same lock.
const MpdPlaylist* mpdPlaylistHandle();
void mpdPlaylistLock();
void mpdPlaylistUnlock();

// Browse holders – same locking discipline.
const MpdBrowseList* mpdBrowseHandle();
const MpdPlaylist*   mpdBrowseSongsHandle();
void mpdBrowseLock();
void mpdBrowseUnlock();

// Stored-playlist stream-name cache: the loaded queue only carries the
// stale live stream tags, so the task keeps a url -> EXTINF name map fed
// from listplaylistinfo.  Returns the station name for a stream URL, or
// an empty String.  Thread-safe (use from the UI task).
String mpdExtinfName(const String& file);