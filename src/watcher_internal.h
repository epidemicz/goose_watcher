/* Shared by the watcher layer's own C file, never by a program: the
   per-watch state, the slot table with generations, errors, and the
   backend entry points the three OS backends implement. The layer is
   thread-safe by construction: per-watch state, no process globals
   beyond the slot table (which its mutex guards) and per-thread error
   text. One watch must only be used by one thread at a time. */

#ifndef GS_WATCH_INTERNAL_H
#define GS_WATCH_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "watcher_api.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef SRWLOCK watch_mutex;
#define WATCH_MUTEX_INIT SRWLOCK_INIT
#define watch_mutex_lock(m) AcquireSRWLockExclusive(m)
#define watch_mutex_unlock(m) ReleaseSRWLockExclusive(m)
#ifdef _MSC_VER
#define WATCH_TLS __declspec(thread)
#else
#define WATCH_TLS __thread
#endif
#else
#include <pthread.h>
typedef pthread_mutex_t watch_mutex;
#define WATCH_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
#define watch_mutex_lock(m) pthread_mutex_lock(m)
#define watch_mutex_unlock(m) pthread_mutex_unlock(m)
#define WATCH_TLS __thread
#endif

/* --- errors --------------------------------------------------------------- */

/* Something outside the program's control failed: the reason is kept, per
   thread, for gs_watch_error. Returns false, for `return watch_fail(...)`,
   or the zero value beside it. */
bool watch_fail(const char *fmt, ...);

/* --- backends ---------------------------------------------------------------
   One static init/wait/drain/close set per platform, called directly
   (no vtable, no hot-loop indirection). `wait` blocks up to timeout_ms
   (-1 forever): 1 where directory activity arrived (drain it), 0 on
   timeout, -1 on fatal error (the reason in watch_fail). `drain`
   harvests touched names into the watch's pending set (ops ORed from the
   platform's own event flags); false is a fatal error. `close` releases
   the OS resources. `watch_touch` and `watch_snapshot` are the shared
   core's, for the backend to call. Compile-time platforms only (#error in
   watcher_poll.c pins them). */

typedef struct watch_state watch_state;

/* Backend entry points, one platform's defined in watcher_poll.c. */
bool watch_be_add(watch_state *w, const char *dir);
int watch_be_wait(watch_state *w, int64_t timeout_ms);
bool watch_be_drain(watch_state *w);
void watch_be_remove_dir(watch_state *w, size_t index);
void watch_be_close(watch_state *w);

#ifdef _WIN32
/* Windows: the per-directory backend resources. One handle/event/packet
   per watched directory, the single set of fields extended into an array. */
typedef struct {
    HANDLE handle;     /* The directory handle ReadDirectoryChangesW reads. */
    HANDLE event;      /* Signalled when a change packet arrives. */
    OVERLAPPED ov;     /* The one outstanding read. */
    uint8_t *packet;   /* The read's destination, WATCH_PACKET bytes. */
    bool pending_read; /* A read is outstanding. */
} watch_win_dir;
#endif

/* --- per-watch state -------------------------------------------------------- */

/* One touched name in the burst set: a copy of the joined name (dir/file,
   joined at watch_touch time from the owning dir) plus the ORed op bits
   the drain observed (GS_WATCH_OP_* values, like fsnotify's newEvent
   accumulation) and, where a rename paired, a copy of the joined old name
   for renamed_from framing (NULL otherwise). */
typedef struct {
    char *name;
    size_t len;
    uint32_t ops;
    char *renamed_from;
    size_t renamed_len;
} watch_touched;

/* A live watch: the set of watched directories, the snapshot name set the
   resolver diffs against, and the pending touched set the current burst
   gathers. The snapshot holds joined dir/file entries (NUL-joined); the
   pending set is an array of watch_touched, each a copied joined name
   plus ORed op bits plus an optional paired old name. */
struct watch_state {
    /* Backend resources (one platform's are live). */
    int fd;            /* Linux: inotify fd. macOS: kqueue fd. Unused on Windows. */
    /* The watched directories, NUL-terminated, without trailing slash,
       in add order; wd_dirs[i] pairs with the native handle below it. */
    char **wd_dirs;
    size_t wd_len;
    size_t wd_cap;
#ifdef __linux__
    int *wd_ids;       /* inotify watch descriptor per wd_dirs entry. */
    bool *wd_dead;     /* Drop-after-frame marks (self event already framed). */
    /* IN_IGNORED arrivals owed to explicit removes (each inotify_rm_watch
       queues one): consumed FIFO, so a reused wd's stale IGNORED never
       drops its new occupant; reset on overflow (queue state unknown). */
    uint32_t stale_ignored;
    /* The inotify cookie LRU for rename pairing (fsnotify's koekje, 10
       slots): move_names holds a joined old name per stored cookie. */
    uint32_t move_cookies[10];
    char *move_names[10];
    uint8_t move_next;
#endif
#ifdef __APPLE__
    int *extra_fds;    /* O_EVTONLY dir fd per wd_dirs entry. */
    bool *wd_dead;     /* Drop-after-frame marks (self event already framed). */
    uint32_t *dir_fflags; /* Accumulated kevent fflags per wd_dirs entry. */
    bool note_pending;       /* A vnode note arrived and is undrained. */
#endif
#ifdef _WIN32
    watch_win_dir *win_dirs; /* Backend resources per wd_dirs entry. */
    bool *wd_dead;     /* Drop-after-frame marks (self event already framed). */
#endif
    /* Sticky kernel-overflow flag (IN_Q_OVERFLOW): gs_watch_next reports
       the burst, then the overflow error on the call after, and the watch
       continues. */
    bool overflowed;
    /* The snapshot: NUL-joined joined names across all watched dirs. */
    char *snapshot;
    size_t snapshot_len;
    size_t snapshot_cap;
    /* The pending touched set of the burst being gathered. */
    watch_touched *touched;
    size_t touched_len;
    size_t touched_cap;
    /* The drain buffer: the Linux inotify read buffer (16384 bytes).
       Sizes are intentionally not synchronized: WATCH_PACKET is 64 KiB
       (Windows packet), this buffer is 16 KiB, and the Goose side drains
       framed bursts with a fixed 64 KiB slice. */
    uint8_t drain[16384];
};

/* Records a touched name, copying dir/file joined (dir is the owning
   watched dir, name its bare entry): the drain buffer is reused across
   reads, but a burst spans several of them. A second hit for the same
   joined name ORs ops in. renamed_from (a joined old name, or NULL with
   len 0) is kept from the first hit that carries one. */
bool watch_touch(watch_state *w, const char *dir, const char *name, size_t len,
                 uint32_t ops, const char *renamed_from, size_t renamed_len);

/* Records a touched joined path directly (for self events naming a
   watched dir itself): the path is already joined, so no dir applies. */
bool watch_touch_joined(watch_state *w, const char *path, size_t len, uint32_t ops);

/* Frees one touched entry's copies (not the entry itself). */
void watch_touched_free(watch_touched *t);

/* True when the joined name was there at snapshot time. */
bool watch_was_there(const watch_state *w, const char *name, size_t len);

/* Whether the joined name is there now (a file or a directory), via
   *there; false with no *there write is out-of-memory (never forged
   silence). */
bool watch_exists_now(watch_state *w, const char *name, size_t len, bool *there);

/* Rebuilds the snapshot from every watched directory's current listing. */
bool watch_snapshot(watch_state *w);

/* Drops the watched dir at index (backend resources, then compaction);
   the watcher itself stays open. */
void watch_drop_dir(watch_state *w, size_t index);

/* Drops every dir marked in wd_dead (after the frame carrying the self
   events); the watcher itself stays open. */
void watch_drop_dead(watch_state *w);

/* Frees everything the state owns (not the state itself). */
void watch_state_free(watch_state *w);

#endif
