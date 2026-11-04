/* The watcher layer's C API: every function watcher.goose reaches through
   an `extern "..." fn`, listed once in GS_WATCH_API. That list expands
   into the prototypes below, and test/api_check.py checks watcher.goose
   against it.

   The layer watches a set of directories per handle and reports each
   change as a framed (path, op, renamed_from) event in a slice the
   program provides. Callbacks become arrays: `gs_watch_next` copies the
   events of one burst into the caller's slice and returns how many there
   were. -2 means the burst exceeded the caller slice; the burst is spent
   and the snapshot advanced, and the Goose side surfaces it as a
   catchable error the watch continues from (fixed 64 KiB drain, the lost
   burst unrecoverable but the watch alive). No builder crosses the
   boundary, and `goose.h` is unneeded; what crosses by value is kept to
   what TinyCC passes the way the C compilers do (scalars and small
   all-integer structs).

   Watchers are slot ids with a generation, stored as an i64: 0 is none,
   and a closed or never-opened id is an error to use, not a crash. Every
   call checks its handle first. All blocking and all fd/handle lifetime
   stays in C; the Goose side owns the delivery above it — the callback
   loop in each_event, and a Poller holding the pending burst plus a
   fixed drain slice in poll.

   Errors: a function that can fail for reasons outside the program (a
   directory that is not there) returns false or a zero handle, with the
   reason in gs_watch_error. The error text is per thread; one watch may
   be used by one thread at a time, and different watches by different
   threads. */

#ifndef GS_WATCH_API_H
#define GS_WATCH_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- slices, handles and ops -------------------------------------------- */

typedef struct { uint8_t *data; int64_t len; } gs_watch_bytes;   /* u8[:], the generated sl_u8 */

typedef struct { int64_t id; } gs_watch_watcher;                  /* Watch: slot id, 0 = none */

/* Both structs are naturally packed (u8*+i64 = 16, i64 = 8): no packing
   pragma. If a TinyCC packing case appears, re-add with that case cited. */

/* --- functions ------------------------------------------------------------ */

#define GS_WATCH_API(X) \
    X(gs_watch_watcher, gs_watch_create, (void)) \
    X(uint8_t, gs_watch_add, (gs_watch_watcher h, gs_watch_bytes path)) \
    X(uint8_t, gs_watch_remove, (gs_watch_watcher h, gs_watch_bytes path)) \
    X(int64_t, gs_watch_watch_list, (gs_watch_watcher h, gs_watch_bytes out)) \
    X(int64_t, gs_watch_next, (gs_watch_watcher h, gs_watch_bytes out, int64_t timeout_ms)) \
    X(void, gs_watch_close, (gs_watch_watcher h)) \
    X(int64_t, gs_watch_error, (gs_watch_bytes out))

#define GS_WATCH_PROTO(ret, name, params) ret name params;
GS_WATCH_API(GS_WATCH_PROTO)
#undef GS_WATCH_PROTO

/* The constants watcher.goose declares, with their Goose types; here
   they are GS_WATCH_<name>. The ops are what gs_watch_next frames
   beside each path. Single-bit only: C frames one op per event; the
   numbers match fsnotify's Op bits so the C drain reads like
   backend_inotify.go. */
#define GS_WATCH_CONSTANTS(X) \
    /* Ops. */ \
    X(i32, OP_CREATE, 1) \
    X(i32, OP_WRITE, 2) \
    X(i32, OP_REMOVE, 4) \
    X(i32, OP_RENAME, 8) \
    X(i32, OP_CHMOD, 16)

#define GS_WATCH_ENUM(type, name, value) GS_WATCH_##name = value,
enum { GS_WATCH_CONSTANTS(GS_WATCH_ENUM) };
#undef GS_WATCH_ENUM

#ifdef __cplusplus
}
#endif

#endif
