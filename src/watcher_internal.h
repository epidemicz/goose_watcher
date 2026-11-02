/* Shared by the watcher layer's own C files, never by a program: the
   per-watch state, the slot table with generations, errors, and the
   backend vtable the three OS backends sit behind. The layer is
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
#define watch_mutex_init(m) InitializeSRWLock(m)
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
#define watch_mutex_init(m) pthread_mutex_init((m), NULL)
#define watch_mutex_lock(m) pthread_mutex_lock(m)
#define watch_mutex_unlock(m) pthread_mutex_unlock(m)
#define WATCH_TLS __thread
#endif

/* --- errors --------------------------------------------------------------- */

/* Something outside the program's control failed: the reason is kept, per
   thread, for gs_watch_error. Returns false, for `return watch_fail(...)`,
   or the zero value beside it. */
bool watch_fail(const char *fmt, ...);

/* --- verbs ------------------------------------------------------------------ */

enum {
    WATCH_CREATED = GS_WATCH_VERB_CREATED,
    WATCH_MODIFIED = GS_WATCH_VERB_MODIFIED,
    WATCH_DELETED = GS_WATCH_VERB_DELETED,
};

/* --- backend vtable -----------------------------------------------------------
   One static vtable per platform, behind which the OS backend hides. `wait`
   blocks up to timeout_ms (-1 forever): 1 where directory activity arrived
   (drain it), 0 on timeout, -1 on fatal error (the reason in watch_fail).
   `drain` harvests touched names into the watch's pending set
   (written=true where contents changed); false is a fatal error. `close`
   releases the OS resources. `watch_touch` and `watch_snapshot` are the
   shared core's, for the backend to call. */

typedef struct watch_state watch_state;

typedef struct {
    bool (*init)(watch_state *w, const char *dir);
    int (*wait)(watch_state *w, int64_t timeout_ms);
    bool (*drain)(watch_state *w);
    void (*close)(watch_state *w);
} watch_backend;

const watch_backend *watch_backend_get(void);

/* --- per-watch state -------------------------------------------------------- */

/* One touched name in the burst set: a copy of the name plus whether any
   content write hit it. */
typedef struct {
    char *name;
    size_t len;
    bool written;
} watch_touched;

/* A live watch: the OS backend's resources, the snapshot name set the
   resolver diffs against, and the pending touched set the current burst
   gathers. The snapshot and pending sets are NUL-joined names; pending
   carries a written flag per name. */
struct watch_state {
    /* Backend resources (one platform's are live). */
    int fd;            /* Linux: inotify fd. macOS: kqueue fd. */
    int extra;         /* Linux: inotify watch descriptor. macOS: O_EVTONLY dir fd. */
#ifdef _WIN32
    HANDLE handle;     /* The directory handle ReadDirectoryChangesW reads. */
    HANDLE event;      /* Signalled when a change packet arrives. */
    OVERLAPPED ov;     /* The one outstanding read. */
    uint8_t *packet;   /* The read's destination, WATCH_PACKET bytes. */
    bool pending_read; /* A read is outstanding. */
#endif
    /* The directory, NUL-terminated, without trailing slash. */
    char *dir;
    /* The snapshot: NUL-joined names at open and after each burst. */
    char *snapshot;
    size_t snapshot_len;
    size_t snapshot_cap;
    /* The pending touched set of the burst being gathered. */
    watch_touched *touched;
    size_t touched_len;
    size_t touched_cap;
    /* The drain buffer: inotify events, kevent slots, or nothing (Windows
       reads into its packet). */
    uint8_t drain[16384];
};

/* Records a touched name, copying it: the drain buffer is reused across
   reads, but a burst spans several of them. A second hit for the same name
   only upgrades written. */
bool watch_touch(watch_state *w, const char *name, size_t len, bool written);

/* True when the name was there at snapshot time. */
bool watch_was_there(const watch_state *w, const char *name, size_t len);

/* True when the name is there now (a file or a directory). */
bool watch_exists_now(watch_state *w, const char *name, size_t len);

/* Rebuilds the snapshot from the directory's current listing. */
bool watch_snapshot(watch_state *w);

/* Frees everything the state owns (not the state itself). */
void watch_state_free(watch_state *w);

#endif
