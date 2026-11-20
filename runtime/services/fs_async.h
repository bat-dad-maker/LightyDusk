// Smooth loading (experimental, Settings > Video, off by default: g_settings.smooth_loading). Read once, the first
// time fs_async_on() is asked, so it cannot change under a running game; when it is off nothing here runs and
// services/fs.c takes exactly the upstream paths.
//
// (a) Read-ahead: after each read of the game's RomFS image, a bounded window past it is handed to a background thread
//     that asks the kernel to bring it into the page cache (posix_fadvise WILLNEED): the next reads find it there.
// (b) Save writes off the game thread: a save file opened for writing is kept in memory (SaveShadow) while it is open;
//     the game's Write / Read / SetSize / GetSize work on that copy. On Flush, Commit or close, a copy of its content
//     is queued for a background thread, which writes <file>.smooth-tmp, fsyncs it, renames it over the file
//     (atomic replace) and fsyncs the folder. Pending writes are flushed when the app goes to the background or exits.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool fs_async_on(void);  // the setting, latched on first use

// (a) read-ahead: fd is the image's (a real file descriptor), off / n the read just made
void fs_async_readahead(int fd, uint64_t off, uint64_t n);

// (b) saves
typedef struct SaveShadow SaveShadow;
// The shadow of path for a file opened for writing (shared by every handle open on it), loaded from fd; NULL when
// the file is too big to keep in memory (then the caller uses fd as upstream does)
SaveShadow *fs_shadow_open(const char *path, int fd);
SaveShadow *fs_shadow_find(const char *path);  // an open one for path (a read-only open sees the newest content), +1 ref
int64_t fs_shadow_read(SaveShadow *s, void *dst, uint64_t n, uint64_t off);
bool fs_shadow_write(SaveShadow *s, const void *src, uint64_t n, uint64_t off);
bool fs_shadow_set_size(SaveShadow *s, uint64_t size);
uint64_t fs_shadow_size(SaveShadow *s);
void fs_shadow_flush(SaveShadow *s);  // queue its content to be written (no wait)
void fs_shadow_close(SaveShadow *s);  // flush if changed, -1 ref
// Commit of a save folder: every changed shadow under root queued
void fs_shadow_commit(const char *root);
// Before the save folder changes by name (delete, rename, clean): the queue written out first, and shadows of what
// goes away dropped (dead) or moved (renamed), so a later flush can't bring a deleted file back
void fs_async_before_unlink(const char *path_or_dir);
void fs_async_before_rename(const char *from, const char *to);
// Wait until nothing for path (or, NULL, nothing at all) is still queued or being written
void fs_async_wait(const char *path);
// Everything queued written out (the app to the background, the app exiting); no-op when off
void fs_async_flush(void);
bool fs_async_pending(void);  // something still queued or being written

#ifdef __cplusplus
}
#endif
