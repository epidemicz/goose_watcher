/* The watcher layer's C API: every function watcher.goose reaches through
   an `extern "..." fn`, listed once in GS_WATCH_API. That list expands
   into the prototypes below, and test/api_check.py checks watcher.goose
   against it.

   The layer watches one directory per handle and reports each change as a
   framed (name, verb) event in a slice the program provides. Callbacks
   become arrays: `gs_watch_next` copies the events of one burst into the
   caller's slice and returns how many there were, so a second call with a
   larger slice gets the rest. No builder crosses the boundary, and
   `goose.h` is unneeded; what crosses by value is kept to what TinyCC
   passes the way the C compilers do (scalars and small all-integer
   structs).

   Watchers are slot ids with a generation, stored as an i64: 0 is none,
   and a closed or never-opened id is an error to use, not a crash. Every
   call checks its handle first. All blocking and all fd/handle lifetime
   stays in C; the Goose side owns only the callback loop plus a fixed
   drain slice.

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

#pragma pack(push, 1)

/* --- slices, handles and verbs ------------------------------------------- */

typedef struct { uint8_t *data; int64_t len; } gs_watch_bytes;   /* u8[:], the generated sl_u8 */

typedef struct { int64_t id; } gs_watch_watcher;                  /* Watch: slot id, 0 = none */

#pragma pack(pop)

/* --- functions ------------------------------------------------------------ */

#define GS_WATCH_API(X) \
    X(uint8_t, gs_watch_available, (void)) \
    X(gs_watch_watcher, gs_watch_open, (gs_watch_bytes dir)) \
    X(int64_t, gs_watch_next, (gs_watch_watcher h, gs_watch_bytes out, int64_t timeout_ms)) \
    X(void, gs_watch_close, (gs_watch_watcher h)) \
    X(int64_t, gs_watch_error, (gs_watch_bytes out))

#define GS_WATCH_PROTO(ret, name, params) ret name params;
GS_WATCH_API(GS_WATCH_PROTO)
#undef GS_WATCH_PROTO

/* The constants watcher.goose declares, with their Goose types; here
   they are GS_WATCH_<name>. The kinds are what gs_watch_next frames
   beside each name. */
#define GS_WATCH_CONSTANTS(X) \
    /* Verbs. */ \
    X(i32, VERB_CREATED, 1) \
    X(i32, VERB_MODIFIED, 2) \
    X(i32, VERB_DELETED, 3)

#define GS_WATCH_ENUM(type, name, value) GS_WATCH_##name = value,
enum { GS_WATCH_CONSTANTS(GS_WATCH_ENUM) };
#undef GS_WATCH_ENUM

#ifdef __cplusplus
}
#endif

#endif
