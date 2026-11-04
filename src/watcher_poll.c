/* The watcher layer: a set of directories watched per handle, each
   change reported as a framed (path, op, renamed_from) event in the
   caller's slice. Three backends with direct static calls, split _WIN32
   first, then __APPLE__ and __linux__ for the POSIX differences. Each
   backend is dependency-free: inotify + poll + read on Linux, kqueue +
   EVFILT_VNODE on macOS, ReadDirectoryChangesW on an event on Windows.

   Out of scope: recursive watches, symlink following. */

#include "watcher_internal.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <poll.h>
#include <sys/inotify.h>
#endif
#endif
#ifdef _WIN32
/* Vista's; TinyCC's and MinGW's winnls.h may predate it. */
#ifndef WC_ERR_INVALID_CHARS
#define WC_ERR_INVALID_CHARS 0x80
#endif
#endif

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
#error "watcher: supported on Linux, macOS and Windows only"
#endif

/* --- errors --------------------------------------------------------------- */

static WATCH_TLS char watch_error_text[1024];

bool watch_fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(watch_error_text, sizeof watch_error_text, fmt, args);
    va_end(args);
    return false;
}

static int64_t watch_copy_text(const char *text, gs_watch_bytes out) {
    int64_t n = text ? (int64_t)strlen(text) : 0;
    int64_t k = n < out.len ? n : out.len;
    if (k > 0) memcpy(out.data, text, (size_t)k);
    return n;
}

int64_t gs_watch_error(gs_watch_bytes out) {
    return watch_copy_text(watch_error_text, out);
}

/* --- slots ------------------------------------------------------------------
   Watchers are slot ids with a generation: id = (generation << 32) | slot.
   Slot 0 never holds a watch, so id 0 is none. Closing a slot bumps its
   generation, so a closed id is an error to use, not a crash. The table is
   mutex-guarded; the states it points at belong to one thread each. */

typedef struct {
    watch_state *state;
    uint32_t generation;
} watch_slot;

static watch_mutex watch_slots_mutex = WATCH_MUTEX_INIT;
static watch_slot *watch_slots;
static size_t watch_slots_cap;
static size_t watch_slots_len; /* one past the highest live slot */

static gs_watch_watcher watch_add(watch_state *state) {
    gs_watch_watcher none = { 0 };
    watch_mutex_lock(&watch_slots_mutex);
    size_t slot = 0;
    for (size_t i = 1; i < watch_slots_len; i++) {
        if (!watch_slots[i].state) {
            slot = i;
            break;
        }
    }
    if (!slot) {
        if (watch_slots_len == watch_slots_cap) {
            size_t cap = watch_slots_cap ? watch_slots_cap * 2 : 16;
            watch_slot *next =
                (watch_slot *)realloc(watch_slots, cap * sizeof *next);
            if (!next) {
                watch_mutex_unlock(&watch_slots_mutex);
                return none;
            }
            memset(next + watch_slots_cap, 0,
                   (cap - watch_slots_cap) * sizeof *next);
            watch_slots = next;
            watch_slots_cap = cap;
        }
        slot = watch_slots_len++;
    }
    if (watch_slots[slot].generation == 0) watch_slots[slot].generation = 1;
    watch_slots[slot].state = state;
    uint64_t id = ((uint64_t)watch_slots[slot].generation << 32) | (uint64_t)slot;
    watch_mutex_unlock(&watch_slots_mutex);
    gs_watch_watcher h = { (int64_t)id };
    return h;
}

/* Decodes a watcher id into its slot and generation, bounds-checked:
   false means a closed or never-opened id (reason in watch_fail). Shared
   by watch_lookup and gs_watch_close (close keeps its exclusive take). */
static bool watch_decode_id(gs_watch_watcher h, uint32_t *slot, uint32_t *gen) {
    uint64_t id = (uint64_t)h.id;
    uint32_t s = (uint32_t)(id & 0xffffffffu);
    watch_mutex_lock(&watch_slots_mutex);
    bool live = s < watch_slots_len && watch_slots[s].state &&
                watch_slots[s].generation == (uint32_t)(id >> 32);
    watch_mutex_unlock(&watch_slots_mutex);
    if (!live) {
        watch_fail("watcher: a watch that was closed, or never opened");
        return false;
    }
    *slot = s;
    *gen = (uint32_t)(id >> 32);
    return true;
}

/* The state for a live id, or NULL (with watch_fail set) for a closed or
   never-opened one. The caller must not hold the mutex. */
static watch_state *watch_lookup(gs_watch_watcher h) {
    uint32_t slot = 0, gen = 0;
    if (!watch_decode_id(h, &slot, &gen)) return NULL;
    watch_mutex_lock(&watch_slots_mutex);
    watch_state *state = watch_slots[slot].state;
    watch_mutex_unlock(&watch_slots_mutex);
    return state;
}

/* --- shared core ------------------------------------------------------------ */

/* One header-inline u32le codec for the framed-burst path (framing) and
   the inotify decode path: single definition, no drift. */
static inline uint32_t watch_u32le_get(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void watch_u32le_put(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* One shared slice guard for the entry points: a negative len, or a
   positive len with no data, is not writable. */
static inline bool watch_slice_ok(gs_watch_bytes out, const char *what) {
    if (out.len < 0 || (out.len > 0 && !out.data)) {
        watch_fail("watcher: %s is not writable", what);
        return false;
    }
    return true;
}

#ifdef _WIN32
/* One UTF-16 helper pair for the four conversion sites. Returns NULL on
   failure (reason in watch_fail); the caller frees the result. */
static wchar_t *watch_utf8_to_wide(const char *text) {
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (units <= 0) {
        watch_fail("watcher: the directory is not valid UTF-8");
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)units * sizeof *wide);
    if (!wide) {
        watch_fail("watcher: out of memory");
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, units);
    return wide;
}

/* Converts wide text (chars units, or -1 for NUL-terminated) to UTF-8:
   true with *out set (the caller frees, *out_len excluding any NUL) on
   success, true with *out NULL where the name is unconvertible (the
   caller skips it, as before), false on OOM (reason in watch_fail). */
static bool watch_wide_to_utf8(const wchar_t *text, int chars, char **out, size_t *out_len) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, chars,
                                NULL, 0, NULL, NULL);
    if (n <= 0) {
        *out = NULL;
        return true;
    }
    char *bytes = (char *)malloc((size_t)n + 1);
    if (!bytes) return watch_fail("watcher: out of memory");
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, chars, bytes, n,
                        NULL, NULL);
    bytes[n] = 0;
    *out = bytes;
    *out_len = (chars < 0) ? (size_t)(n - 1) : (size_t)n;
    return true;
}
#endif

/* Joins dir and a bare entry name with one '/': byte-concatenation, no
   normalization, the exact Event.path rule. dir is the owning entry of
   wd_dirs (never freed through here). */
static bool watch_join(char **out, size_t *out_len, const char *dir,
                       const char *name, size_t len) {
    size_t dirlen = strlen(dir);
    char *joined = (char *)malloc(dirlen + 1 + len + 1);
    if (!joined) return watch_fail("watcher: out of memory");
    memcpy(joined, dir, dirlen);
    joined[dirlen] = '/';
    memcpy(joined + dirlen + 1, name, len);
    joined[dirlen + 1 + len] = 0;
    *out = joined;
    *out_len = dirlen + 1 + len;
    return true;
}

void watch_touched_free(watch_touched *t) {
    free(t->name);
    free(t->renamed_from);
    t->name = NULL;
    t->renamed_from = NULL;
    t->len = 0;
    t->renamed_len = 0;
    t->ops = 0;
}

bool watch_touch(watch_state *w, const char *dir, const char *name, size_t len,
                 uint32_t ops, const char *renamed_from, size_t renamed_len) {
    char *joined = NULL;
    size_t jlen = 0;
    if (!watch_join(&joined, &jlen, dir, name, len)) return false;
    for (size_t i = 0; i < w->touched_len; i++) {
        if (w->touched[i].len == jlen && memcmp(w->touched[i].name, joined, jlen) == 0) {
            free(joined);
            w->touched[i].ops |= ops;
            if (renamed_len && !w->touched[i].renamed_from) {
                char *copy = (char *)malloc(renamed_len + 1);
                if (!copy) return watch_fail("watcher: out of memory");
                memcpy(copy, renamed_from, renamed_len);
                copy[renamed_len] = 0;
                w->touched[i].renamed_from = copy;
                w->touched[i].renamed_len = renamed_len;
            }
            return true;
        }
    }
    if (w->touched_len == w->touched_cap) {
        size_t cap = w->touched_cap ? w->touched_cap * 2 : 16;
        watch_touched *next = (watch_touched *)realloc(w->touched, cap * sizeof *next);
        if (!next) {
            free(joined);
            return watch_fail("watcher: out of memory");
        }
        w->touched = next;
        w->touched_cap = cap;
    }
    char *old = NULL;
    if (renamed_len) {
        old = (char *)malloc(renamed_len + 1);
        if (!old) {
            free(joined);
            return watch_fail("watcher: out of memory");
        }
        memcpy(old, renamed_from, renamed_len);
        old[renamed_len] = 0;
    }
    w->touched[w->touched_len].name = joined;
    w->touched[w->touched_len].len = jlen;
    w->touched[w->touched_len].ops = ops;
    w->touched[w->touched_len].renamed_from = old;
    w->touched[w->touched_len].renamed_len = renamed_len;
    w->touched_len++;
    return true;
}

bool watch_touch_joined(watch_state *w, const char *path, size_t len, uint32_t ops) {
    for (size_t i = 0; i < w->touched_len; i++) {
        if (w->touched[i].len == len && memcmp(w->touched[i].name, path, len) == 0) {
            w->touched[i].ops |= ops;
            return true;
        }
    }
    if (w->touched_len == w->touched_cap) {
        size_t cap = w->touched_cap ? w->touched_cap * 2 : 16;
        watch_touched *next = (watch_touched *)realloc(w->touched, cap * sizeof *next);
        if (!next) return watch_fail("watcher: out of memory");
        w->touched = next;
        w->touched_cap = cap;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) return watch_fail("watcher: out of memory");
    memcpy(copy, path, len);
    copy[len] = 0;
    w->touched[w->touched_len].name = copy;
    w->touched[w->touched_len].len = len;
    w->touched[w->touched_len].ops = ops;
    w->touched[w->touched_len].renamed_from = NULL;
    w->touched[w->touched_len].renamed_len = 0;
    w->touched_len++;
    return true;
}

bool watch_was_there(const watch_state *w, const char *name, size_t len) {
    size_t pos = 0;
    while (pos < w->snapshot_len) {
        const char *entry = w->snapshot + pos;
        size_t left = w->snapshot_len - pos;
        const char *end = (const char *)memchr(entry, 0, left);
        if (!end) return false;
        size_t n = (size_t)(end - entry);
        if (n == len && memcmp(entry, name, len) == 0) return true;
        pos += n + 1;
    }
    return false;
}

bool watch_exists_now(watch_state *w, const char *name, size_t len, bool *there) {
    (void)w;
    char *path = (char *)malloc(len + 1);
    if (!path) return watch_fail("watcher: out of memory");
    memcpy(path, name, len);
    path[len] = 0;
#ifdef _WIN32
    wchar_t *wp = watch_utf8_to_wide(path);
    free(path);
    if (!wp) return false;
    *there = GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
    free(wp);
    return true;
#else
    struct stat st;
    *there = stat(path, &st) == 0;
    free(path);
    return true;
#endif
}

/* Appends a name to the snapshot being rebuilt. */
static bool watch_snap_add(watch_state *w, const char *name, size_t len) {
    if (w->snapshot_len + len + 1 > w->snapshot_cap) {
        size_t cap = w->snapshot_cap ? w->snapshot_cap * 2 : 1024;
        while (cap < w->snapshot_len + len + 1) cap *= 2;
        char *next = (char *)realloc(w->snapshot, cap);
        if (!next) return watch_fail("watcher: out of memory");
        w->snapshot = next;
        w->snapshot_cap = cap;
    }
    memcpy(w->snapshot + w->snapshot_len, name, len);
    w->snapshot[w->snapshot_len + len] = 0;
    w->snapshot_len += len + 1;
    return true;
}

/* Snapshots one watched directory's listing, joining entries at add time. */
static bool watch_snapshot_dir(watch_state *w, const char *dir) {
#ifdef _WIN32
    /* dir\* in UTF-16, listed with FindFirstFileW. */
    size_t dirlen = strlen(dir);
    char *pattern = (char *)malloc(dirlen + 3);
    if (!pattern) return watch_fail("watcher: out of memory");
    memcpy(pattern, dir, dirlen);
    memcpy(pattern + dirlen, "\\*", 3);
    wchar_t *wp = watch_utf8_to_wide(pattern);
    free(pattern);
    if (!wp) return false;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(wp, &found);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        return watch_fail("watcher: cannot list the directory");
    }
    bool ok = true;
    do {
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        char *name = NULL;
        size_t nlen = 0;
        if (!watch_wide_to_utf8(found.cFileName, -1, &name, &nlen)) {
            ok = false;
            break;
        }
        if (!name) continue;
        char *joined = NULL;
        size_t jlen = 0;
        ok = watch_join(&joined, &jlen, dir, name, nlen) &&
             watch_snap_add(w, joined, jlen);
        free(joined);
        free(name);
        if (!ok) break;
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return ok;
#else
    DIR *d = opendir(dir);
    if (!d) return watch_fail("watcher: cannot list the directory");
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            if (errno) ok = watch_fail("watcher: cannot list the directory");
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *joined = NULL;
        size_t jlen = 0;
        ok = watch_join(&joined, &jlen, dir, e->d_name, strlen(e->d_name)) &&
             watch_snap_add(w, joined, jlen);
        free(joined);
        if (!ok) break;
    }
    closedir(d);
    return ok;
#endif
}

bool watch_snapshot(watch_state *w) {
    w->snapshot_len = 0;
    for (size_t i = 0; i < w->wd_len; i++) {
        /* The watched dir itself is a member: a self event (DELETE_SELF
           on rm -rf) resolves !now && was to Remove instead of net-zero
           silence. Untouched members never frame, so this costs nothing
           per burst. */
        if (!watch_snap_add(w, w->wd_dirs[i], strlen(w->wd_dirs[i]))) return false;
        if (!watch_snapshot_dir(w, w->wd_dirs[i])) return false;
    }
    return true;
}

/* Frees the slot at index in every parallel array except wd_dirs itself
   (the caller frees or compacts that one); backend resources first. */
static void watch_free_slot(watch_state *w, size_t index) {
    watch_be_remove_dir(w, index);
    free(w->wd_dirs[index]);
}

void watch_drop_dir(watch_state *w, size_t index) {
    if (index >= w->wd_len) return;
    watch_free_slot(w, index);
    for (size_t i = index + 1; i < w->wd_len; i++) {
        w->wd_dirs[i - 1] = w->wd_dirs[i];
#ifdef __linux__
        w->wd_ids[i - 1] = w->wd_ids[i];
        w->wd_dead[i - 1] = w->wd_dead[i];
#endif
#ifdef __APPLE__
        w->extra_fds[i - 1] = w->extra_fds[i];
        w->wd_dead[i - 1] = w->wd_dead[i];
        w->dir_fflags[i - 1] = w->dir_fflags[i];
#endif
#ifdef _WIN32
        w->win_dirs[i - 1] = w->win_dirs[i];
        w->wd_dead[i - 1] = w->wd_dead[i];
#endif
    }
    w->wd_len--;
}

void watch_drop_dead(watch_state *w) {
    for (size_t i = w->wd_len; i-- > 0;) {
        bool dead = false;
#ifdef __linux__
        dead = w->wd_dead[i];
#endif
#ifdef __APPLE__
        dead = w->wd_dead[i];
#endif
#ifdef _WIN32
        dead = w->wd_dead[i];
#endif
        if (dead) watch_drop_dir(w, i);
    }
}

void watch_state_free(watch_state *w) {
    for (size_t i = 0; i < w->wd_len; i++) free(w->wd_dirs[i]);
    free(w->wd_dirs);
#ifdef __linux__
    free(w->wd_ids);
    free(w->wd_dead);
    for (size_t i = 0; i < 10; i++) free(w->move_names[i]);
#endif
#ifdef __APPLE__
    free(w->extra_fds);
    free(w->wd_dead);
    free(w->dir_fflags);
#endif
#ifdef _WIN32
    /* win_dirs' packets stay with the backend close (Windows owns them
       there): freeing them here as well would double-free. */
    free(w->win_dirs);
    free(w->wd_dead);
#endif
    free(w->snapshot);
    for (size_t i = 0; i < w->touched_len; i++) watch_touched_free(&w->touched[i]);
    free(w->touched);
}

/* Resolves one touched name to its single op from the net change: there
   now vs there at snapshot, with the observed bits tipping survivors.
   Precedence is the folding contract: creation subsumes a same-window
   write/chmod, write dominates chmod, a lone rename-out reads as
   Rename, and a survivor touched by a create or a remove — a
   delete-then-recreate, or a move-in landing on an existing name (an
   atomic save) — reports Write, since it carries new contents.
   Names that came and went (editor temps, created then deleted in one
   window) resolve to 0 and report nothing. OOM propagates as -1
   (callers treat it as fatal, never as silence). */
static int watch_resolve(watch_state *w, const watch_touched *t) {
    bool now = false;
    if (!watch_exists_now(w, t->name, t->len, &now)) return -1;
    bool was = watch_was_there(w, t->name, t->len);
    if (now && !was) return GS_WATCH_OP_CREATE;
    if (!now && was) {
        if (t->ops & GS_WATCH_OP_RENAME) return GS_WATCH_OP_RENAME;
        return GS_WATCH_OP_REMOVE;
    }
    if (now && (t->ops & GS_WATCH_OP_WRITE)) return GS_WATCH_OP_WRITE;
    if (now && (t->ops & (GS_WATCH_OP_CREATE | GS_WATCH_OP_REMOVE))) return GS_WATCH_OP_WRITE;
    if (now && (t->ops & GS_WATCH_OP_CHMOD)) return GS_WATCH_OP_CHMOD;
    return 0;
}

/* Frames the burst's resolved events into out: u32 path_len LE, u32 op
   LE, u32 renamed_len LE, path bytes, renamed bytes. Returns the event
   count, or -2 (with the required byte count in out[0..8] only if
   out.len >= 8; if out.len < 8 no size is delivered) when out is too
   small: the burst is spent and the snapshot advanced either way, a lost
   burst the Goose side surfaces as a catchable error the watch continues
   from. */
static int64_t watch_frame(watch_state *w, gs_watch_bytes out) {
    /* Resolve first: came-and-went names frame nothing. */
    uint32_t *ops = NULL;
    if (w->touched_len) {
        ops = (uint32_t *)malloc(w->touched_len * sizeof *ops);
        /* Like the -2 path below, spend and re-snapshot before returning
           -1, so the next call never re-resolves a stale burst against a
           stale snapshot. */
        if (!ops) {
            for (size_t i = 0; i < w->touched_len; i++) watch_touched_free(&w->touched[i]);
            w->touched_len = 0;
            if (!watch_snapshot(w)) { /* reason already in watch_fail */ }
            watch_fail("watcher: out of memory");
            return -1;
        }
    }
    int64_t count = 0;
    int64_t need = 0;
    bool failed = false;
    for (size_t i = 0; i < w->touched_len; i++) {
        int op = watch_resolve(w, &w->touched[i]);
        if (op < 0) {
            failed = true;
            break;
        }
        ops[i] = (uint32_t)op;
        if (op) {
            count++;
            need += 12 + (int64_t)w->touched[i].len + (int64_t)w->touched[i].renamed_len;
        }
    }
    int64_t rc = failed ? -1 : count;
    if (!failed && need > out.len) {
        if (out.len >= 8) {
            watch_u32le_put(out.data, (uint32_t)(need & 0xffffffffu));
            watch_u32le_put(out.data + 4, (uint32_t)((uint64_t)need >> 32));
        }
        /* The -2 reason lives here (not Goose-side): the Goose side would
           need a duplicate constant, and error() must report it through
           the same channel as every other reason. */
        watch_fail("watcher: burst larger than the drain");
        rc = -2;
    } else if (!failed) {
        int64_t at = 0;
        for (size_t i = 0; i < w->touched_len; i++) {
            if (!ops[i]) continue;
            watch_u32le_put(out.data + at, (uint32_t)w->touched[i].len);
            watch_u32le_put(out.data + at + 4, ops[i]);
            watch_u32le_put(out.data + at + 8, (uint32_t)w->touched[i].renamed_len);
            memcpy(out.data + at + 12, w->touched[i].name, w->touched[i].len);
            if (w->touched[i].renamed_len) {
                memcpy(out.data + at + 12 + w->touched[i].len,
                       w->touched[i].renamed_from, w->touched[i].renamed_len);
            }
            at += 12 + (int64_t)w->touched[i].len + (int64_t)w->touched[i].renamed_len;
        }
    }
    for (size_t i = 0; i < w->touched_len; i++) watch_touched_free(&w->touched[i]);
    w->touched_len = 0;
    /* Self events named dirs that are gone: drop them after the frame
       carrying them, so the watcher itself stays open on the rest. */
    watch_drop_dead(w);
    if (!watch_snapshot(w)) rc = -1;
    free(ops);
    return rc;
}
/* --- backends --------------------------------------------------------------- */

#ifdef _WIN32

#define WATCH_PACKET (64 * 1024)
/* Sizes are intentionally not synchronized: WATCH_PACKET is 64 KiB
   (Windows packet), watch_state.drain is 16 KiB (Linux inotify read
   buffer), and the Goose side drains framed bursts with a fixed 64 KiB
   slice. */

static bool watch_win_start_read(watch_state *w, size_t index) {
    watch_win_dir *d = &w->win_dirs[index];
    if (d->pending_read) return true;
    memset(&d->ov, 0, sizeof d->ov);
    d->ov.hEvent = d->event;
    DWORD back = 0;
    if (ReadDirectoryChangesW(d->handle, d->packet, WATCH_PACKET, FALSE,
                              FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                              &back, &d->ov, NULL)) {
        d->pending_read = true;
        return true;
    }
    if (GetLastError() == ERROR_IO_PENDING) {
        d->pending_read = true;
        return true;
    }
    if (GetLastError() == ERROR_ACCESS_DENIED) {
        /* The watched dir itself went away (fsnotify readEvents): a self
           Remove plus drop-after-frame, not a fatal error. */
        const char *dir = w->wd_dirs[index];
        if (!watch_touch_joined(w, dir, strlen(dir), GS_WATCH_OP_REMOVE)) return false;
        w->wd_dead[index] = true;
        return true;
    }
    return watch_fail("watcher: ReadDirectoryChangesW failed");
}

bool watch_be_add(watch_state *w, const char *dir) {
    wchar_t *wp = watch_utf8_to_wide(dir);
    if (!wp) return false;
    HANDLE handle = CreateFileW(wp, FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                NULL);
    free(wp);
    if (handle == INVALID_HANDLE_VALUE)
        return watch_fail("watcher: cannot watch '%s': missing or not a directory", dir);
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    uint8_t *packet = (uint8_t *)malloc(WATCH_PACKET);
    if (!event || !packet) {
        if (event) CloseHandle(event);
        free(packet);
        CloseHandle(handle);
        return watch_fail("watcher: out of memory");
    }
    if (w->wd_len == w->wd_cap) {
        size_t cap = w->wd_cap ? w->wd_cap * 2 : 4;
        char **dirs = (char **)realloc(w->wd_dirs, cap * sizeof *dirs);
        watch_win_dir *wins = (watch_win_dir *)realloc(w->win_dirs, cap * sizeof *wins);
        bool *dead = (bool *)realloc(w->wd_dead, cap * sizeof *dead);
        if (!dirs || !wins || !dead) {
            free(dirs ? NULL : NULL);
            CloseHandle(event);
            free(packet);
            CloseHandle(handle);
            return watch_fail("watcher: out of memory");
        }
        w->wd_dirs = dirs;
        w->win_dirs = wins;
        w->wd_dead = dead;
        w->wd_cap = cap;
    }
    char *copy = _strdup(dir);
    if (!copy) {
        CloseHandle(event);
        free(packet);
        CloseHandle(handle);
        return watch_fail("watcher: out of memory");
    }
    size_t index = w->wd_len;
    w->wd_dirs[index] = copy;
    w->win_dirs[index].handle = handle;
    w->win_dirs[index].event = event;
    memset(&w->win_dirs[index].ov, 0, sizeof w->win_dirs[index].ov);
    w->win_dirs[index].packet = packet;
    w->win_dirs[index].pending_read = false;
    w->wd_dead[index] = false;
    w->wd_len++;
    if (!watch_win_start_read(w, index)) {
        watch_drop_dir(w, index);
        return false;
    }
    return true;
}

int watch_be_wait(watch_state *w, int64_t timeout_ms) {
    for (size_t i = 0; i < w->wd_len; i++) {
        if (!watch_win_start_read(w, i)) return -1;
    }
    if (!w->wd_len) {
        /* Nothing watched: sleep the timeout out (fsnotify has no empty
           set, but remove() makes one here). */
        DWORD ms = timeout_ms < 0 ? INFINITE : timeout_ms > 0xfffffffe ? 0xfffffffe : (DWORD)timeout_ms;
        DWORD r = WaitForSingleObject(GetCurrentThread(), ms);
        (void)r;
        return 0;
    }
    /* WaitForMultipleObjects caps at 64: chunk the set (fsnotify polls
       one handle per watch the same way). */
    DWORD ms = timeout_ms < 0 ? INFINITE : timeout_ms > 0xfffffffe ? 0xfffffffe : (DWORD)timeout_ms;
    for (size_t base = 0; base < w->wd_len; base += 64) {
        size_t n = w->wd_len - base < 64 ? w->wd_len - base : 64;
        HANDLE events[64];
        for (size_t i = 0; i < n; i++) events[i] = w->win_dirs[base + i].event;
        DWORD r = WaitForMultipleObjects((DWORD)n, events, FALSE, base ? 0 : ms);
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) return 1;
        if (r == WAIT_TIMEOUT) return 0;
        if (r == WAIT_FAILED) {
            watch_fail("watcher: wait failed");
            return -1;
        }
    }
    return 0;
}

bool watch_be_drain(watch_state *w) {
    for (size_t index = 0; index < w->wd_len; index++) {
        watch_win_dir *d = &w->win_dirs[index];
        DWORD got = 0;
        if (!GetOverlappedResult(d->handle, &d->ov, &got, FALSE)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_INCOMPLETE) continue;
            if (e == ERROR_ACCESS_DENIED || e == ERROR_DIRECTORY) {
                /* The watched dir itself went away (fsnotify
                   readEvents): a self Remove plus drop-after-frame. */
                const char *dir = w->wd_dirs[index];
                if (!watch_touch_joined(w, dir, strlen(dir), GS_WATCH_OP_REMOVE)) return false;
                w->wd_dead[index] = true;
                continue;
            }
            return watch_fail("watcher: the directory change read failed");
        }
        d->pending_read = false;
        ResetEvent(d->event);
        /* Harvest this packet, then start the next read before resolving,
           so a change landing mid-resolve is not lost. */
        uint8_t *p = d->packet;
        uint8_t *end = d->packet + got;
        bool ok = true;
        /* Rename pairing within the packet (fsnotify's watch.rename):
           OLD remembers the old name for the NEW half's renamed_from. */
        char *rename_old = NULL;
        size_t rename_old_len = 0;
        while (ok && p + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= end) {
            FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)p;
            int chars = (int)(info->FileNameLength / sizeof(WCHAR));
            char *name = NULL;
            size_t nlen = 0;
            if (!watch_wide_to_utf8(info->FileName, chars, &name, &nlen)) {
                ok = false;
                break;
            }
            if (name) {
                /* The name is relative; subdirectories report their own
                   name. Action mapping per fsnotify newEvent: Windows
                   never emits Chmod. */
                uint32_t ops = 0;
                const char *old = NULL;
                size_t old_len = 0;
                switch (info->Action) {
                case FILE_ACTION_ADDED:
                    ops = GS_WATCH_OP_CREATE;
                    break;
                case FILE_ACTION_REMOVED:
                    ops = GS_WATCH_OP_REMOVE;
                    break;
                case FILE_ACTION_MODIFIED:
                    ops = GS_WATCH_OP_WRITE;
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    ops = GS_WATCH_OP_RENAME;
                    free(rename_old);
                    rename_old = NULL;
                    rename_old_len = 0;
                    if (!watch_join(&rename_old, &rename_old_len,
                                    w->wd_dirs[index], name, nlen)) {
                        free(name);
                        ok = false;
                    }
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    ops = GS_WATCH_OP_CREATE;
                    old = rename_old;
                    old_len = rename_old_len;
                    break;
                default:
                    break;
                }
                if (ok && ops) {
                    ok = watch_touch(w, w->wd_dirs[index], name, nlen,
                                     ops, old, old_len);
                }
                free(name);
                if (!ok) break;
            }
            if (!info->NextEntryOffset) break;
            if (info->NextEntryOffset > (DWORD)(end - p)) break;
            p += info->NextEntryOffset;
        }
        free(rename_old);
        if (ok) ok = watch_win_start_read(w, index);
        if (!ok) return false;
    }
    return true;
}

void watch_be_remove_dir(watch_state *w, size_t index) {
    watch_win_dir *d = &w->win_dirs[index];
    if (d->pending_read) CancelIoEx(d->handle, &d->ov);
    if (d->event) CloseHandle(d->event);
    free(d->packet);
    d->packet = NULL;
    d->event = NULL;
    if (d->handle != INVALID_HANDLE_VALUE && d->handle != NULL) CloseHandle(d->handle);
    d->handle = NULL;
}

void watch_be_close(watch_state *w) {
    for (size_t i = 0; i < w->wd_len; i++) watch_be_remove_dir(w, i);
}

#elif defined(__APPLE__)

/* macOS: kqueue, EVFILT_VNODE on an O_EVTONLY dir fd per watched dir. No
   FSEvents (latency, UUID hassle), no CoreFoundation link. A vnode note
   says something changed, not what: the snapshot diff resolves the kind,
   and the NOTE_* mask marks the candidates. Child renames keep reading
   as Remove+Create halves: no per-file rename note reaches the dir-fd
   backend, so renamed_from stays empty here (structural, not a bug). */

bool watch_be_add(watch_state *w, const char *dir) {
    int dfd = open(dir, O_EVTONLY);
    if (dfd < 0)
        return watch_fail("watcher: cannot watch '%s': missing or not a directory", dir);
    if (w->fd < 0) {
        w->fd = kqueue();
        if (w->fd < 0) {
            close(dfd);
            return watch_fail("watcher: kqueue failed");
        }
    }
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)dfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_DELETE,
           0, NULL);
    if (kevent(w->fd, &ev, 1, NULL, 0, NULL) != 0) {
        close(dfd);
        return watch_fail("watcher: kevent failed");
    }
    if (w->wd_len == w->wd_cap) {
        size_t cap = w->wd_cap ? w->wd_cap * 2 : 4;
        char **dirs = (char **)realloc(w->wd_dirs, cap * sizeof *dirs);
        int *fds = (int *)realloc(w->extra_fds, cap * sizeof *fds);
        bool *dead = (bool *)realloc(w->wd_dead, cap * sizeof *dead);
        uint32_t *flags = (uint32_t *)realloc(w->dir_fflags, cap * sizeof *flags);
        if (!dirs || !fds || !dead || !flags) {
            close(dfd);
            return watch_fail("watcher: out of memory");
        }
        w->wd_dirs = dirs;
        w->extra_fds = fds;
        w->wd_dead = dead;
        w->dir_fflags = flags;
        w->wd_cap = cap;
    }
    char *copy = strdup(dir);
    if (!copy) {
        close(dfd);
        return watch_fail("watcher: out of memory");
    }
    w->wd_dirs[w->wd_len] = copy;
    w->extra_fds[w->wd_len] = dfd;
    w->wd_dead[w->wd_len] = false;
    w->dir_fflags[w->wd_len] = 0;
    w->wd_len++;
    return true;
}

int watch_be_wait(watch_state *w, int64_t timeout_ms) {
    if (!w->wd_len) {
        /* Nothing watched: sleep the timeout out (no fd to wait on). */
        struct timespec ts, *tp = NULL;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000;
            tp = &ts;
        }
        struct kevent ev;
        int n = w->fd >= 0 ? kevent(w->fd, NULL, 0, &ev, 1, tp) : 0;
        if (n < 0) {
            watch_fail("watcher: kevent wait failed");
            return -1;
        }
        return 0;
    }
    struct timespec ts, *tp = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        tp = &ts;
    }
    struct kevent ev;
    int n;
    do {
        n = kevent(w->fd, NULL, 0, &ev, 1, tp);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        watch_fail("watcher: kevent wait failed");
        return -1;
    }
    if (n == 0) return 0;
    /* Accumulate the fflags against the owning dir: the ident is its fd. */
    for (size_t i = 0; i < w->wd_len; i++) {
        if ((uintptr_t)w->extra_fds[i] == ev.ident) {
            w->dir_fflags[i] |= ev.fflags;
            break;
        }
    }
    w->note_pending = true;
    return 1;
}

bool watch_be_drain(watch_state *w) {
    if (!w->note_pending) return true;
    /* Drain every pending note (last-note-wins would drop write marks):
       a zero-timeout kevent loop until no event remains. */
    struct timespec zero = { 0, 0 };
    for (;;) {
        struct kevent ev;
        int n = kevent(w->fd, NULL, 0, &ev, 1, &zero);
        if (n < 0) {
            if (errno == EINTR) continue;
            return watch_fail("watcher: kevent wait failed");
        }
        if (n == 0) break;
        for (size_t i = 0; i < w->wd_len; i++) {
            if ((uintptr_t)w->extra_fds[i] == ev.ident) {
                w->dir_fflags[i] |= ev.fflags;
                break;
            }
        }
    }
    w->note_pending = false;
    /* kqueue names nothing: diff each dir's listing against the snapshot
       and touch every difference. NOTE mapping per fsnotify newEvent,
       with the Write+Remove -> Remove strip. */
    for (size_t index = 0; index < w->wd_len; index++) {
        uint32_t fflags = w->dir_fflags[index];
        w->dir_fflags[index] = 0;
        if (!fflags) continue;
        const char *dir = w->wd_dirs[index];
        if (fflags & (NOTE_DELETE | NOTE_RENAME)) {
            /* The watched dir itself went away (fsnotify readEvents
               removes it): a self Remove/Rename plus drop-after-frame. */
            uint32_t self = (fflags & NOTE_DELETE) ? GS_WATCH_OP_REMOVE : GS_WATCH_OP_RENAME;
            if (!watch_touch_joined(w, dir, strlen(dir), self)) return false;
            w->wd_dead[index] = true;
            continue;
        }
        uint32_t ops = 0;
        if (fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_LINK)) ops |= GS_WATCH_OP_WRITE;
        if (fflags & NOTE_ATTRIB) ops |= GS_WATCH_OP_CHMOD;
        DIR *d = opendir(dir);
        if (!d) return watch_fail("watcher: cannot list the directory");
        bool ok = true;
        /* Current names not in the snapshot: created (or survivors the
           listing cannot tell apart — the resolver tips written ones). */
        for (;;) {
            errno = 0;
            struct dirent *e = readdir(d);
            if (!e) {
                if (errno) ok = watch_fail("watcher: cannot list the directory");
                break;
            }
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            size_t len = strlen(e->d_name);
            char *joined = NULL;
            size_t jlen = 0;
            if (!watch_join(&joined, &jlen, dir, e->d_name, len)) {
                ok = false;
                break;
            }
            bool was = watch_was_there(w, joined, jlen);
            free(joined);
            if (!was) {
                if (!watch_touch(w, dir, e->d_name, len, ops, NULL, 0)) {
                    ok = false;
                    break;
                }
            } else if (ops) {
                /* A survivor under a note: maybe changed. Touch it so
                   the resolver sees the bits; untouched survivors stay
                   silent. */
                if (!watch_touch(w, dir, e->d_name, len, ops, NULL, 0)) {
                    ok = false;
                    break;
                }
            }
        }
        closedir(d);
        if (!ok) return false;
        /* Snapshot names under this dir gone now: removed. */
        size_t dirlen = strlen(dir);
        size_t pos = 0;
        while (pos < w->snapshot_len) {
            const char *entry = w->snapshot + pos;
            size_t left = w->snapshot_len - pos;
            const char *end = (const char *)memchr(entry, 0, left);
            if (!end) break;
            size_t n = (size_t)(end - entry);
            if (n > dirlen + 1 && !memcmp(entry, dir, dirlen) && entry[dirlen] == '/') {
                bool there = false;
                if (!watch_exists_now(w, entry, n, &there)) return false;
                /* The Write+Remove -> Remove strip (fsnotify newEvent):
                   a deleted child reads as Remove, never Write. */
                if (!there && !watch_touch(w, dir, entry + dirlen + 1,
                                           n - dirlen - 1, GS_WATCH_OP_REMOVE, NULL, 0)) return false;
            }
            pos += n + 1;
        }
    }
    return true;
}

void watch_be_remove_dir(watch_state *w, size_t index) {
    if (w->extra_fds[index] >= 0) close(w->extra_fds[index]);
    w->extra_fds[index] = -1;
}

void watch_be_close(watch_state *w) {
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
    for (size_t i = 0; i < w->wd_len; i++) {
        if (w->extra_fds[i] >= 0) close(w->extra_fds[i]);
        w->extra_fds[i] = -1;
    }
}

#else /* __linux__ */

/* Linux: inotify + poll + read. The decode loop reads inotify_event
   records: u32le wd at +0, u32le mask at +4, u32le cookie at +8, u32le
   len at +12, NUL name after the 16-byte header; the mask records op
   bits per hit, single-op collapse happens in watch_resolve. */

#define WATCH_MASK \
    (IN_CREATE | IN_MODIFY | IN_ATTRIB | IN_DELETE | IN_DELETE_SELF | \
     IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF | IN_Q_OVERFLOW | IN_IGNORED)

/* fsnotify's default mask plus the two already carried (DELETE_SELF /
   MOVE_SELF feed the self events). */

bool watch_be_add(watch_state *w, const char *dir) {
    if (w->fd < 0) {
        w->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (w->fd < 0) return watch_fail("watcher: inotify_init1 failed (need Linux with inotify)");
    }
    int wd = inotify_add_watch(w->fd, dir, WATCH_MASK);
    if (wd < 0)
        return watch_fail("watcher: cannot watch '%s': missing or not a directory", dir);
    if (w->wd_len == w->wd_cap) {
        size_t cap = w->wd_cap ? w->wd_cap * 2 : 4;
        char **dirs = (char **)realloc(w->wd_dirs, cap * sizeof *dirs);
        int *ids = (int *)realloc(w->wd_ids, cap * sizeof *ids);
        bool *dead = (bool *)realloc(w->wd_dead, cap * sizeof *dead);
        if (!dirs || !ids || !dead) {
            inotify_rm_watch(w->fd, wd);
            return watch_fail("watcher: out of memory");
        }
        w->wd_dirs = dirs;
        w->wd_ids = ids;
        w->wd_dead = dead;
        w->wd_cap = cap;
    }
    char *copy = strdup(dir);
    if (!copy) {
        inotify_rm_watch(w->fd, wd);
        return watch_fail("watcher: out of memory");
    }
    w->wd_dirs[w->wd_len] = copy;
    w->wd_ids[w->wd_len] = wd;
    w->wd_dead[w->wd_len] = false;
    w->wd_len++;
    return true;
}

int watch_be_wait(watch_state *w, int64_t timeout_ms) {
    if (w->fd < 0) {
        /* Nothing ever added: sleep the timeout out (no fd to poll). */
        int ms = timeout_ms < 0 ? -1 : timeout_ms > 0x7fffffff ? 0x7fffffff : (int)timeout_ms;
        if (ms < 0) {
            /* Forever with nothing to wait on: block until a signal
               (poll on no fds with -1 sleeps indefinitely). */
            int r;
            do {
                r = poll(NULL, 0, -1);
            } while (r < 0 && errno == EINTR);
            return 0;
        }
        struct pollfd pfd = { -1, 0, 0 };
        (void)pfd;
        int r;
        do {
            r = poll(NULL, 0, ms);
        } while (r < 0 && errno == EINTR);
        if (r < 0) {
            watch_fail("watcher: poll failed");
            return -1;
        }
        return 0;
    }
    struct pollfd pfd = { w->fd, POLLIN, 0 };
    int ms = timeout_ms < 0 ? -1 : timeout_ms > 0x7fffffff ? 0x7fffffff : (int)timeout_ms;
    int r;
    do {
        r = poll(&pfd, 1, ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        watch_fail("watcher: poll failed");
        return -1;
    }
    return r > 0 ? 1 : 0;
}

/* Stores a MOVED_FROM cookie -> joined old name pair in the LRU (fsnotify's
   koekje, 10 slots); the MOVED_TO half resolves it for renamed_from. */
static bool watch_move_store(watch_state *w, uint32_t cookie, const char *dir,
                             const char *name, size_t len) {
    if (!cookie) return true;
    char *joined = NULL;
    size_t jlen = 0;
    if (!watch_join(&joined, &jlen, dir, name, len)) return false;
    size_t slot = w->move_next++ % 10;
    free(w->move_names[slot]);
    w->move_cookies[slot] = cookie;
    w->move_names[slot] = joined;
    (void)jlen;
    return true;
}

/* Resolves a MOVED_TO cookie to its stored joined old name, or NULL on a
   miss (plain Create). Consumes the slot, so a cookie never pairs twice. */
static const char *watch_move_lookup(watch_state *w, uint32_t cookie, size_t *len) {
    if (!cookie) return NULL;
    for (size_t i = 0; i < 10; i++) {
        if (w->move_cookies[i] == cookie && w->move_names[i]) {
            w->move_cookies[i] = 0;
            *len = strlen(w->move_names[i]);
            return w->move_names[i];
        }
    }
    return NULL;
}

bool watch_be_drain(watch_state *w) {
    for (;;) {
        ssize_t n = read(w->fd, w->drain, sizeof w->drain);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            return watch_fail("watcher: inotify read failed");
        }
        if (n == 0) return true;
        size_t off = 0;
        while (off + 16 <= (size_t)n) {
            int wd = (int)watch_u32le_get(w->drain + off);
            uint32_t mask = watch_u32le_get(w->drain + off + 4);
            uint32_t cookie = watch_u32le_get(w->drain + off + 8);
            uint32_t len = watch_u32le_get(w->drain + off + 12);
            if (off + 16 + len > (size_t)n) break;
            size_t index = w->wd_len;
            for (size_t i = 0; i < w->wd_len; i++) {
                if (w->wd_ids[i] == wd) {
                    index = i;
                    break;
                }
            }
            if (mask & IN_Q_OVERFLOW) {
                /* Kernel overflow (fsnotify readEvents): a sticky
                   catchable error the watch continues from, not fatal.
                   The queue state is unknown from here: no IGNORED
                   debt can be trusted. */
                w->overflowed = true;
                w->stale_ignored = 0;
            } else if ((mask & IN_IGNORED) && index < w->wd_len) {
                /* IN_IGNORED is state cleanup, never an event (fsnotify).
                   Owed to an explicit remove, it is that remove's own
                   echo: consume one debt. Otherwise it follows a
                   DELETE_SELF/MOVE_SELF the code already handled: mark
                   the wd dead silently, so a reused wd's stale IGNORED
                   never drops its new occupant's dir. */
                if (w->stale_ignored) {
                    w->stale_ignored--;
                } else {
                    w->wd_dead[index] = true;
                }
            } else if ((mask & IN_IGNORED) && w->stale_ignored) {
                w->stale_ignored--;
            } else if (index < w->wd_len && (mask & (IN_DELETE_SELF | IN_MOVE_SELF))) {
                /* The watched dir itself went away (fsnotify: Remove on
                   DELETE_SELF, Rename on MOVE_SELF, then drop): a self
                   event plus drop-after-frame, so the frame carrying it
                   still emits. Watching both a dir and its parent
                   reports the Remove twice (once per watch): the same
                   information fsnotify dedups, kept here as two events. */
                uint32_t self = (mask & IN_DELETE_SELF) ? GS_WATCH_OP_REMOVE : GS_WATCH_OP_RENAME;
                const char *dir = w->wd_dirs[index];
                if (!watch_touch_joined(w, dir, strlen(dir), self)) return false;
                w->wd_dead[index] = true;
            } else if (index < w->wd_len && len) {
                /* The name is NUL-terminated inside its len bytes;
                   other nameless events (the watch's own lifetime)
                   touch nothing. */
                const char *raw = (const char *)(w->drain + off + 16);
                const char *z = (const char *)memchr(raw, 0, len);
                size_t nlen = z ? (size_t)(z - raw) : len;
                if (nlen > 0) {
                    const char *dir = w->wd_dirs[index];
                    uint32_t ops = 0;
                    const char *old = NULL;
                    size_t old_len = 0;
                    if (mask & IN_MODIFY) ops |= GS_WATCH_OP_WRITE;
                    if (mask & IN_ATTRIB) ops |= GS_WATCH_OP_CHMOD;
                    if (mask & IN_CREATE) ops |= GS_WATCH_OP_CREATE;
                    if (mask & IN_DELETE) ops |= GS_WATCH_OP_REMOVE;
                    if (mask & IN_MOVED_FROM) {
                        ops |= GS_WATCH_OP_RENAME;
                        if (!watch_move_store(w, cookie, dir, raw, nlen)) return false;
                    }
                    if (mask & IN_MOVED_TO) {
                        /* A move-in carries new contents for its target:
                           where it lands on an existing name (an atomic
                           save), that survivor resolves Write; where it
                           lands new, Create with the paired old name. */
                        ops |= GS_WATCH_OP_CREATE;
                        old = watch_move_lookup(w, cookie, &old_len);
                    }
                    if (ops && !watch_touch(w, dir, raw, nlen, ops, old, old_len)) return false;
                }
            }
            off += 16 + len;
        }
    }
}

void watch_be_remove_dir(watch_state *w, size_t index) {
    if (w->fd >= 0 && index < w->wd_len && w->wd_ids[index] >= 0) {
        inotify_rm_watch(w->fd, w->wd_ids[index]);
        w->wd_ids[index] = -1;
    }
}

void watch_be_close(watch_state *w) {
    if (w->fd >= 0) close(w->fd);
    w->fd = -1;
}

#endif

/* --- entry points ------------------------------------------------------------- */

#define WATCH_QUIET_MS 100

/* Normalizes a user path for the watched set: NUL-terminated copy without
   trailing slash (but never the root's own). On Windows both '/' and '\'
   strip, and a drive root (X:/ or X:\) keeps its slash: collapsing it
   would make an invalid X:. An empty path is passed through and fails at the backend with
   cannot watch, like any missing path. comp_len excludes the NUL. */
static bool watch_normalize_path(const char *data, size_t len, char **out, size_t *comp_len) {
    char *path = (char *)malloc(len + 1);
    if (!path) return watch_fail("watcher: out of memory");
    if (len > 0) memcpy(path, data, len);
    path[len] = 0;
    size_t plen = len;
#ifdef _WIN32
    bool is_drive_root = plen == 3 && path[1] == ':' &&
                         (path[2] == '/' || path[2] == '\\');
    while (plen > 1 && !is_drive_root &&
           (path[plen - 1] == '/' || path[plen - 1] == '\\')) {
        path[--plen] = 0;
        is_drive_root = plen == 3 && path[1] == ':' &&
                        (path[2] == '/' || path[2] == '\\');
    }
#else
    while (plen > 1 && path[plen - 1] == '/') path[--plen] = 0;
#endif
    *out = path;
    *comp_len = plen;
    return true;
}

/* The watched-set index of path (comparison length comp_len), or wd_len. */
static size_t watch_find_dir(const watch_state *w, const char *path, size_t comp_len) {
    for (size_t i = 0; i < w->wd_len; i++) {
        if (strlen(w->wd_dirs[i]) == comp_len && !memcmp(w->wd_dirs[i], path, comp_len)) return i;
    }
    return w->wd_len;
}

gs_watch_watcher gs_watch_create(void) {
    gs_watch_watcher none = { 0 };
    watch_state *w = (watch_state *)calloc(1, sizeof *w);
    if (!w) {
        watch_fail("watcher: out of memory");
        return none;
    }
    w->fd = -1;
    gs_watch_watcher h = watch_add(w);
    if (!h.id) {
        watch_state_free(w);
        free(w);
        watch_fail("watcher: out of memory");
        return none;
    }
    return h;
}

uint8_t gs_watch_add(gs_watch_watcher h, gs_watch_bytes path) {
    watch_state *w = watch_lookup(h);
    if (!w) return 0;
    if (!watch_slice_ok(path, "the path slice")) return 0;
    if (path.len > 0 && memchr(path.data, 0, (size_t)path.len)) {
        watch_fail("watcher: the directory holds a NUL byte");
        return 0;
    }
    char *norm = NULL;
    size_t comp_len = 0;
    if (!watch_normalize_path((const char *)path.data, (size_t)path.len, &norm, &comp_len)) return 0;
    /* Duplicate add: idempotent no-op (fsnotify register ORs flags and
       returns the existing watch; no mask variants exist here, so
       nothing to OR). */
    /* A dead-but-undropped path from a self event in an unframed burst:
       the drop lands with the frame; re-add before it re-watches. */
    if (watch_find_dir(w, norm, comp_len) < w->wd_len) {
        free(norm);
        return 1;
    }
    if (!watch_be_add(w, norm)) {
        free(norm);
        return 0;
    }
    free(norm);
    if (!watch_snapshot(w)) {
        /* The add half-landed; unwind it so the set stays truthful. */
        watch_drop_dir(w, w->wd_len - 1);
        return 0;
    }
    return 1;
}

uint8_t gs_watch_remove(gs_watch_watcher h, gs_watch_bytes path) {
    watch_state *w = watch_lookup(h);
    if (!w) return 0;
    if (!watch_slice_ok(path, "the path slice")) return 0;
    if (path.len > 0 && memchr(path.data, 0, (size_t)path.len)) {
        watch_fail("watcher: the path holds a NUL byte");
        return 0;
    }
    char *norm = NULL;
    size_t comp_len = 0;
    if (!watch_normalize_path((const char *)path.data, (size_t)path.len, &norm, &comp_len)) return 0;
    size_t index = watch_find_dir(w, norm, comp_len);
    if (index >= w->wd_len) {
        watch_fail("fsnotify: can't remove non-existent watch: %s", norm);
        free(norm);
        return 0;
    }
    /* An explicit remove queues its own IN_IGNORED echo (fsnotify's
       remove path sees the same one): owe one debt so the drain
       consumes it instead of dropping a reused wd's new occupant. */
#ifdef __linux__
    w->stale_ignored++;
#endif
    watch_drop_dir(w, index);
    free(norm);
    if (!watch_snapshot(w)) return 0;
    return 1;
}

int64_t gs_watch_watch_list(gs_watch_watcher h, gs_watch_bytes out) {
    watch_state *w = watch_lookup(h);
    if (!w) return -1;
    if (!watch_slice_ok(out, "the drain slice")) return -1;
    int64_t need = 0;
    for (size_t i = 0; i < w->wd_len; i++) need += (int64_t)strlen(w->wd_dirs[i]) + 1;
    int64_t k = need < out.len ? need : out.len;
    int64_t at = 0;
    for (size_t i = 0; i < w->wd_len && at < k; i++) {
        size_t n = strlen(w->wd_dirs[i]);
        int64_t room = k - at;
        int64_t cp = (int64_t)n < room ? (int64_t)n : room;
        if (cp > 0) memcpy(out.data + at, w->wd_dirs[i], (size_t)cp);
        at += cp;
        if (at < k) {
            out.data[at++] = 0;
        }
    }
    return need;
}

int64_t gs_watch_next(gs_watch_watcher h, gs_watch_bytes out, int64_t timeout_ms) {
    watch_state *w = watch_lookup(h);
    if (!w) return -1;
    if (!watch_slice_ok(out, "the drain slice")) return -1;
    /* A sticky kernel overflow surfaces on the call after the burst that
       carried it (fsnotify: the error beside the events, then continue):
       the burst already framed; this call reports the loss and clears. */
    if (w->overflowed) {
        w->overflowed = false;
        watch_fail("fsnotify: queue or buffer overflow");
        return -1;
    }
    /* The first wait honors the caller's timeout; the quiet window
       gathers the burst once anything arrives. */
    int r = watch_be_wait(w, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    for (;;) {
        if (!watch_be_drain(w)) return -1;
        /* Quiet for QUIET_MS means the burst is over; anything more
           keeps gathering. A negative wait is a real error. */
        r = watch_be_wait(w, WATCH_QUIET_MS);
        if (r < 0) return -1;
        if (r == 0) break;
    }
    if (!w->touched_len && !w->overflowed) return 0;
    /* The burst frames first; the overflow flag waits for the next call. */
    return watch_frame(w, out);
}

void gs_watch_close(gs_watch_watcher h) {
    if (!h.id) return;
    uint32_t slot = 0, gen = 0;
    if (!watch_decode_id(h, &slot, &gen)) return;
    watch_mutex_lock(&watch_slots_mutex);
    watch_state *state = NULL;
    if (slot < watch_slots_len && watch_slots[slot].state &&
        watch_slots[slot].generation == gen) {
        state = watch_slots[slot].state;
        watch_slots[slot].state = NULL;
        if (++watch_slots[slot].generation == 0) watch_slots[slot].generation = 1;
        while (watch_slots_len > 1 && !watch_slots[watch_slots_len - 1].state)
            watch_slots_len--;
    }
    watch_mutex_unlock(&watch_slots_mutex);
    if (!state) return;
    watch_be_close(state);
    watch_state_free(state);
    free(state);
}
