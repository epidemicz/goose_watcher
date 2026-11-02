/* The watcher layer: one directory watched per handle, each change
   reported as a framed (name, verb) event in the caller's slice. Three
   backends behind one static vtable, split exactly like the runtime's
   (runtime_os.h): _WIN32 first, then __APPLE__ and __linux__ for the POSIX
   differences. Each backend is dependency-free: inotify + poll + read on
   Linux, kqueue + EVFILT_VNODE on macOS, ReadDirectoryChangesW on an event
   on Windows (kernel32 only).

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
/* Vista's; TinyCC's and MinGW's winnls.h may predate it (runtime_os.h). */
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

uint8_t gs_watch_available(void) { return 1; }

int64_t gs_watch_error(gs_watch_bytes out) {
    return watch_copy_text(watch_error_text, out);
}

/* --- slots ------------------------------------------------------------------
   Handles are slot ids with a generation: id = (generation << 32) | slot.
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

static gs_watch_handle watch_add(watch_state *state) {
    gs_watch_handle none = { 0 };
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
    gs_watch_handle h = { (int64_t)id };
    return h;
}

/* The state for a live id, or NULL (with watch_fail set) for a closed or
   never-opened one. The caller must not hold the mutex. */
static watch_state *watch_lookup(gs_watch_handle h) {
    uint64_t id = (uint64_t)h.id;
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t gen = (uint32_t)(id >> 32);
    watch_mutex_lock(&watch_slots_mutex);
    watch_state *state = NULL;
    if (slot < watch_slots_len && watch_slots[slot].state &&
        watch_slots[slot].generation == gen)
        state = watch_slots[slot].state;
    watch_mutex_unlock(&watch_slots_mutex);
    if (!state)
        watch_fail("watcher: a watch that was closed, or never opened");
    return state;
}

/* --- shared core ------------------------------------------------------------ */

bool watch_touch(watch_state *w, const char *name, size_t len, bool written) {
    for (size_t i = 0; i < w->touched_len; i++) {
        if (w->touched[i].len == len && memcmp(w->touched[i].name, name, len) == 0) {
            if (written) w->touched[i].written = true;
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
    memcpy(copy, name, len);
    copy[len] = 0;
    w->touched[w->touched_len].name = copy;
    w->touched[w->touched_len].len = len;
    w->touched[w->touched_len].written = written;
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

bool watch_exists_now(watch_state *w, const char *name, size_t len) {
    size_t dirlen = strlen(w->dir);
    char *path = (char *)malloc(dirlen + 1 + len + 1);
    if (!path) return false;
    memcpy(path, w->dir, dirlen);
    path[dirlen] = '/';
    memcpy(path + dirlen + 1, name, len);
    path[dirlen + 1 + len] = 0;
#ifdef _WIN32
    /* UTF-8 to UTF-16, the gs_os_native pattern in runtime_os.h. */
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    bool there = false;
    if (units > 0) {
        wchar_t *wp = (wchar_t *)malloc((size_t)units * sizeof *wp);
        if (wp) {
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wp, units))
                there = GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
            free(wp);
        }
    }
    free(path);
    return there;
#else
    struct stat st;
    bool there = stat(path, &st) == 0;
    free(path);
    return there;
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

bool watch_snapshot(watch_state *w) {
    w->snapshot_len = 0;
#ifdef _WIN32
    /* dir\* in UTF-16, listed with FindFirstFileW. */
    size_t dirlen = strlen(w->dir);
    char *pattern = (char *)malloc(dirlen + 3);
    if (!pattern) return watch_fail("watcher: out of memory");
    memcpy(pattern, w->dir, dirlen);
    memcpy(pattern + dirlen, "/*", 3);
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, NULL, 0);
    if (units <= 0) {
        free(pattern);
        return watch_fail("watcher: the directory is not valid UTF-8");
    }
    wchar_t *wp = (wchar_t *)malloc((size_t)units * sizeof *wp);
    if (!wp) {
        free(pattern);
        return watch_fail("watcher: out of memory");
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pattern, -1, wp, units);
    free(pattern);
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
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, found.cFileName, -1,
                                    NULL, 0, NULL, NULL);
        if (n <= 1) continue;
        char *name = (char *)malloc((size_t)n);
        if (!name) {
            ok = watch_fail("watcher: out of memory");
            break;
        }
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, found.cFileName, -1, name, n,
                            NULL, NULL);
        ok = watch_snap_add(w, name, (size_t)n - 1);
        free(name);
        if (!ok) break;
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return ok;
#else
    DIR *d = opendir(w->dir);
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
        if (!watch_snap_add(w, e->d_name, strlen(e->d_name))) {
            ok = false;
            break;
        }
    }
    closedir(d);
    return ok;
#endif
}

void watch_state_free(watch_state *w) {
    free(w->dir);
    free(w->snapshot);
    for (size_t i = 0; i < w->touched_len; i++) free(w->touched[i].name);
    free(w->touched);
}

/* Resolves one touched name to its verb from the net change: there now vs
   there at snapshot, with a content write tipping a survivor to modified.
   Names that came and went (editor temps) resolve to 0 and report nothing.
   Ported verbatim from the demo's resolve. */
static int watch_resolve(watch_state *w, const watch_touched *t) {
    bool now = watch_exists_now(w, t->name, t->len);
    bool was = watch_was_there(w, t->name, t->len);
    if (now && !was) return WATCH_CREATED;
    if (!now && was) return WATCH_DELETED;
    if (now && t->written) return WATCH_MODIFIED;
    return 0;
}

static void watch_put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

/* Frames the burst's resolved events into out: u32 name_len LE, u32 verb
   LE, name bytes. Returns the event count, or -2 with the required byte
   count written to out[0..8] when out is too small. */
static int64_t watch_frame(watch_state *w, gs_watch_bytes out) {
    /* Resolve first: came-and-went names frame nothing. */
    int *verbs = (int *)malloc(w->touched_len * sizeof *verbs);
    if (w->touched_len && !verbs) {
        watch_fail("watcher: out of memory");
        return -1;
    }
    int64_t count = 0;
    int64_t need = 0;
    for (size_t i = 0; i < w->touched_len; i++) {
        verbs[i] = watch_resolve(w, &w->touched[i]);
        if (verbs[i]) {
            count++;
            need += 8 + (int64_t)w->touched[i].len;
        }
    }
    int64_t rc = count;
    if (need > out.len) {
        if (out.len >= 8) {
            watch_put_u32le(out.data, (uint32_t)(need & 0xffffffffu));
            watch_put_u32le(out.data + 4, (uint32_t)((uint64_t)need >> 32));
        }
        rc = -2;
    } else {
        int64_t at = 0;
        for (size_t i = 0; i < w->touched_len; i++) {
            if (!verbs[i]) continue;
            watch_put_u32le(out.data + at, (uint32_t)w->touched[i].len);
            watch_put_u32le(out.data + at + 4, (uint32_t)verbs[i]);
            memcpy(out.data + at + 8, w->touched[i].name, w->touched[i].len);
            at += 8 + (int64_t)w->touched[i].len;
        }
    }
    /* The burst is spent either way: a -2 retry re-resolves nothing. */
    for (size_t i = 0; i < w->touched_len; i++) free(w->touched[i].name);
    w->touched_len = 0;
    if (!watch_snapshot(w)) rc = -1;
    free(verbs);
    return rc;
}
/* --- backends --------------------------------------------------------------- */

#ifdef _WIN32

#define WATCH_PACKET (64 * 1024)

static bool watch_win_start_read(watch_state *w) {
    if (w->pending_read) return true;
    memset(&w->ov, 0, sizeof w->ov);
    w->ov.hEvent = w->event;
    DWORD back = 0;
    if (ReadDirectoryChangesW(w->handle, w->packet, WATCH_PACKET, FALSE,
                              FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                              &back, &w->ov, NULL)) {
        w->pending_read = true;
        return true;
    }
    if (GetLastError() == ERROR_IO_PENDING) {
        w->pending_read = true;
        return true;
    }
    return watch_fail("watcher: ReadDirectoryChangesW failed");
}

static bool watch_win_init(watch_state *w, const char *dir) {
    int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir, -1, NULL, 0);
    if (units <= 0) return watch_fail("watcher: the directory is not valid UTF-8");
    wchar_t *wp = (wchar_t *)malloc((size_t)units * sizeof *wp);
    if (!wp) return watch_fail("watcher: out of memory");
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir, -1, wp, units);
    w->handle = CreateFileW(wp, FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                            NULL);
    free(wp);
    if (w->handle == INVALID_HANDLE_VALUE)
        return watch_fail("watcher: cannot watch '%s': missing or not a directory", dir);
    w->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->packet = (uint8_t *)malloc(WATCH_PACKET);
    if (!w->event || !w->packet) {
        if (w->event) CloseHandle(w->event);
        free(w->packet);
        CloseHandle(w->handle);
        return watch_fail("watcher: out of memory");
    }
    w->pending_read = false;
    return watch_win_start_read(w);
}

static int watch_win_wait(watch_state *w, int64_t timeout_ms) {
    if (!watch_win_start_read(w)) return -1;
    DWORD ms = timeout_ms < 0 ? INFINITE : timeout_ms > 0xfffffffe ? 0xfffffffe : (DWORD)timeout_ms;
    DWORD r = WaitForSingleObject(w->event, ms);
    if (r == WAIT_OBJECT_0) return 1;
    if (r == WAIT_TIMEOUT) return 0;
    watch_fail("watcher: wait failed");
    return -1;
}

static bool watch_win_drain(watch_state *w) {
    DWORD got = 0;
    if (!GetOverlappedResult(w->handle, &w->ov, &got, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE) return true;
        return watch_fail("watcher: the directory change read failed");
    }
    w->pending_read = false;
    ResetEvent(w->event);
    /* Harvest this packet, then start the next read before resolving, so a
       change landing mid-resolve is not lost. */
    uint8_t *p = w->packet;
    uint8_t *end = w->packet + got;
    bool ok = true;
    while (ok && p + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= end) {
        FILE_NOTIFY_INFORMATION *info = (FILE_NOTIFY_INFORMATION *)p;
        int chars = (int)(info->FileNameLength / sizeof(WCHAR));
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info->FileName, chars,
                                    NULL, 0, NULL, NULL);
        if (n > 0) {
            char *name = (char *)malloc((size_t)n);
            if (!name) {
                ok = watch_fail("watcher: out of memory");
                break;
            }
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, info->FileName, chars,
                                name, n, NULL, NULL);
            /* The name is relative; subdirectories report their own name. */
            ok = watch_touch(w, name, (size_t)n,
                             info->Action == FILE_ACTION_MODIFIED ||
                                 info->Action == FILE_ACTION_ADDED ||
                                 info->Action == FILE_ACTION_RENAMED_NEW_NAME);
            free(name);
        }
        if (!info->NextEntryOffset) break;
        if (info->NextEntryOffset > (DWORD)(end - p)) break;
        p += info->NextEntryOffset;
    }
    if (ok) ok = watch_win_start_read(w);
    return ok;
}

static void watch_win_close(watch_state *w) {
    if (w->pending_read) CancelIoEx(w->handle, &w->ov);
    if (w->event) CloseHandle(w->event);
    free(w->packet);
    if (w->handle != INVALID_HANDLE_VALUE && w->handle != NULL) CloseHandle(w->handle);
}

static const watch_backend watch_win_backend = {
    watch_win_init, watch_win_wait, watch_win_drain, watch_win_close,
};

const watch_backend *watch_backend_get(void) { return &watch_win_backend; }

#elif defined(__APPLE__)

/* macOS: kqueue, EVFILT_VNODE on an O_EVTONLY dir fd. No FSEvents (latency,
   UUID hassle), no CoreFoundation link. A vnode note says something
   changed, not what: the snapshot diff resolves the verb, and NOTE_WRITE
   or NOTE_EXTEND marks every candidate written. */

static bool watch_kq_init(watch_state *w, const char *dir) {
    w->extra = open(dir, O_EVTONLY);
    if (w->extra < 0)
        return watch_fail("watcher: cannot watch '%s': missing or not a directory", dir);
    w->fd = kqueue();
    if (w->fd < 0) {
        close(w->extra);
        return watch_fail("watcher: kqueue failed");
    }
    struct kevent ev;
    EV_SET(&ev, (uintptr_t)w->extra, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_DELETE,
           0, NULL);
    if (kevent(w->fd, &ev, 1, NULL, 0, NULL) != 0) {
        close(w->fd);
        close(w->extra);
        return watch_fail("watcher: kevent failed");
    }
    return true;
}

static int watch_kq_wait(watch_state *w, int64_t timeout_ms) {
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
    /* Stash the fflags for the drain: written where contents changed. */
    w->drain[0] = (uint8_t)(ev.fflags & 0xff);
    w->drain[1] = (uint8_t)((ev.fflags >> 8) & 0xff);
    w->drain[2] = (uint8_t)((ev.fflags >> 16) & 0xff);
    w->drain[3] = (uint8_t)((ev.fflags >> 24) & 0xff);
    w->drain[4] = 1; /* a note is pending */
    return 1;
}

static bool watch_kq_drain(watch_state *w) {
    if (!w->drain[4]) return true;
    w->drain[4] = 0;
    uint32_t fflags =
        (uint32_t)w->drain[0] | ((uint32_t)w->drain[1] << 8) |
        ((uint32_t)w->drain[2] << 16) | ((uint32_t)w->drain[3] << 24);
    (void)fflags;
    /* kqueue names nothing: diff the listing against the snapshot and
       touch every difference. NOTE_WRITE/EXTEND means contents changed
       somewhere; conservatively every candidate counts as written. */
    bool written = (fflags & (NOTE_WRITE | NOTE_EXTEND)) != 0;
    DIR *d = opendir(w->dir);
    if (!d) return watch_fail("watcher: cannot list the directory");
    bool ok = true;
    /* Current names not in the snapshot: created (or modified survivors
       the listing cannot tell apart — the resolver tips written ones). */
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            if (errno) ok = watch_fail("watcher: cannot list the directory");
            break;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t len = strlen(e->d_name);
        if (!watch_was_there(w, e->d_name, len)) {
            if (!watch_touch(w, e->d_name, len, written)) {
                ok = false;
                break;
            }
        } else if (written) {
            /* A survivor under a write note: maybe modified. Touch it so
               the resolver sees the write; untouched survivors stay silent. */
            if (!watch_touch(w, e->d_name, len, true)) {
                ok = false;
                break;
            }
        }
    }
    closedir(d);
    if (!ok) return false;
    /* Snapshot names gone now: deleted. */
    size_t pos = 0;
    while (pos < w->snapshot_len) {
        const char *entry = w->snapshot + pos;
        size_t left = w->snapshot_len - pos;
        const char *end = (const char *)memchr(entry, 0, left);
        if (!end) break;
        size_t n = (size_t)(end - entry);
        if (!watch_exists_now(w, entry, n) && !watch_touch(w, entry, n, false))
            return false;
        pos += n + 1;
    }
    return true;
}

static void watch_kq_close(watch_state *w) {
    if (w->fd >= 0) close(w->fd);
    if (w->extra >= 0) close(w->extra);
}

static const watch_backend watch_kq_backend = {
    watch_kq_init, watch_kq_wait, watch_kq_drain, watch_kq_close,
};

const watch_backend *watch_backend_get(void) { return &watch_kq_backend; }

#else /* __linux__ */

/* Linux: inotify + poll + read. The decode loop is the demo's, ported:
   u32le header, NUL name, MODIFY|MOVED_TO sets written. */

#define WATCH_MASK \
    (IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CREATE | IN_DELETE)

static bool watch_in_init(watch_state *w, const char *dir) {
    w->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (w->fd < 0) return watch_fail("watcher: inotify_init1 failed (need Linux with inotify)");
    w->extra = inotify_add_watch(w->fd, dir, WATCH_MASK);
    if (w->extra < 0) {
        close(w->fd);
        return watch_fail("watcher: cannot watch '%s': missing or not a directory", dir);
    }
    return true;
}

static int watch_in_wait(watch_state *w, int64_t timeout_ms) {
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

static uint32_t watch_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool watch_in_drain(watch_state *w) {
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
            uint32_t mask = watch_u32le(w->drain + off + 4);
            uint32_t len = watch_u32le(w->drain + off + 12);
            if (off + 16 + len > (size_t)n) break;
            /* The name is NUL-terminated inside its len bytes; nameless
               events (queue overflow, the watch's own lifetime) touch
               nothing. */
            if (len) {
                const char *raw = (const char *)(w->drain + off + 16);
                const char *z = (const char *)memchr(raw, 0, len);
                size_t nlen = z ? (size_t)(z - raw) : len;
                if (nlen > 0) {
                    /* A move-in carries new contents for its target:
                       where it lands on an existing name (an atomic
                       save), that survivor resolves to modified. */
                    bool wrote = (mask & (IN_MODIFY | IN_MOVED_TO)) != 0;
                    if (!watch_touch(w, raw, nlen, wrote)) return false;
                }
            }
            off += 16 + len;
        }
    }
}

static void watch_in_close(watch_state *w) {
    if (w->fd >= 0) close(w->fd);
}

static const watch_backend watch_in_backend = {
    watch_in_init, watch_in_wait, watch_in_drain, watch_in_close,
};

const watch_backend *watch_backend_get(void) { return &watch_in_backend; }

#endif

/* --- entry points ------------------------------------------------------------- */

#define WATCH_QUIET_MS 100

gs_watch_handle gs_watch_open(gs_watch_bytes dir) {
    gs_watch_handle none = { 0 };
    if (dir.len < 0 || (dir.len > 0 && !dir.data)) {
        watch_fail("watcher: the directory is not valid UTF-8");
        return none;
    }
    if (dir.len > 0 && memchr(dir.data, 0, (size_t)dir.len)) {
        watch_fail("watcher: the directory holds a NUL byte");
        return none;
    }
    char *path = (char *)malloc((size_t)dir.len + 1);
    if (!path) {
        watch_fail("watcher: out of memory");
        return none;
    }
    if (dir.len > 0) memcpy(path, dir.data, (size_t)dir.len);
    path[dir.len] = 0;
    /* A trailing slash would double in existence checks; strip it (but
       never the root's own). */
    size_t plen = (size_t)dir.len;
    while (plen > 1 && path[plen - 1] == '/') path[--plen] = 0;

    watch_state *w = (watch_state *)calloc(1, sizeof *w);
    if (!w) {
        free(path);
        watch_fail("watcher: out of memory");
        return none;
    }
    w->fd = -1;
    w->extra = -1;
    w->dir = path;

    const watch_backend *be = watch_backend_get();
    if (!be->init(w, path) || !watch_snapshot(w)) {
        be->close(w);
        watch_state_free(w);
        free(w);
        return none;
    }
    gs_watch_handle h = watch_add(w);
    if (!h.id) {
        be->close(w);
        watch_state_free(w);
        free(w);
        watch_fail("watcher: out of memory");
        return none;
    }
    return h;
}

int64_t gs_watch_next(gs_watch_handle h, gs_watch_bytes out, int64_t timeout_ms) {
    watch_state *w = watch_lookup(h);
    if (!w) return -1;
    if (out.len < 0 || (out.len > 0 && !out.data)) {
        watch_fail("watcher: the drain slice is not writable");
        return -1;
    }
    const watch_backend *be = watch_backend_get();
    /* The first wait honors the caller's timeout; the quiet window
       gathers the burst once anything arrives. */
    int r = be->wait(w, timeout_ms);
    if (r < 0) return -1;
    if (r == 0) return 0;
    for (;;) {
        if (!be->drain(w)) return -1;
        /* Quiet for QUIET_MS means the burst is over; anything more
           keeps gathering. A negative wait is a real error. */
        r = be->wait(w, WATCH_QUIET_MS);
        if (r < 0) return -1;
        if (r == 0) break;
    }
    if (!w->touched_len) return 0;
    return watch_frame(w, out);
}

void gs_watch_close(gs_watch_handle h) {
    if (!h.id) return;
    watch_mutex_lock(&watch_slots_mutex);
    uint64_t id = (uint64_t)h.id;
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t gen = (uint32_t)(id >> 32);
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
    watch_backend_get()->close(state);
    watch_state_free(state);
    free(state);
}
