// Watches one directory and reports each change as an Event, where
// kind is Created, Modified, or Deleted and describes the net change.
// An atomic save (write temp, rename over) reports Modified on the real
// file; short-lived temp files report nothing.
//
//   import watcher from "./watcher";
//   fn main() {
//       var w, err = watcher::open(".");
//       guard err.len == 0 else { print("watch: ", err); return; }
//       w.each_event() { ev => print(ev.path, " - ", watcher::kind_name(ev.kind)); };
//   }
//
// Changes surface after a short quiet window (100ms), so a burst of
// writes still reports one change per file. A rename reads as its two
// halves: the old path Deleted, the new one Created. Reads and
// metadata-only touches report nothing; subdirectories are not watched
// recursively. The callback returning false stops the watch cleanly.
//
// Event.path is relative to the watched root: a bare name today, a
// subpath ("sub/file") once recursion lands, so recursion will change
// values, not this API. Event.path borrows the drain buffer for the
// duration of the callback: use it inside the callback, or copy it if
// it must outlive the callback (copy it, e.g. into a fixed array
// to cross a thread queue).
//
// The functions are C, the layer in src/ over the OS's own directory
// notifications (inotify on Linux, kqueue on macOS,
// ReadDirectoryChangesW on Windows), declared here as extern fns against
// the list in src/watcher_api.h; test/api_check.py checks the two agree.
// All blocking and all fd/handle lifetime stays in C; this file owns
// only the callback loop plus a fixed drain slice. One watch must only
// be used by one thread at a time; different watches may live on
// different threads.

namespace watcher;

import binary;

// --- kinds: the public event taxonomy ------------------------------------
// Past tense, because an event is a net change after the quiet window,
// not a raw OS write. A rename is Deleted + Created, not a separate
// kind: the snapshot-diff layer cannot pair renames reliably yet.

enum Kind { Created, Modified, Deleted }

// --- structs: the types that cross to C, and the event -------------------
// Watcher is the C layer's slot id, 0 = none. Single-use: open a new
// one to restart. Event is Goose-side only: it never crosses to C.

struct Watcher { id: i64 }

struct Event { path: u8[:], kind: Kind }

// --- the library ---------------------------------------------------------------

// Whether the watcher layer is linked in. A compiler without it refuses to
// run a watcher program in-process, so this is true wherever it runs.
extern fn available() -> bool = "gs_watch_available";
extern fn open_handle(dir: const u8[:]) -> Watcher = "gs_watch_open";
extern fn next_into(h: Watcher, out: u8[:], timeout_ms: i64) -> i64 = "gs_watch_next";
extern fn close_handle(h: Watcher) = "gs_watch_close";
extern fn error_into(out: u8[:]) -> i64 = "gs_watch_error";

// Why the last call that failed did, as text.
fn error() -> u8[>..] {
    var text: u8[>..] = [];
    text.resize(1024, 0);
    let n = error_into(text[..]);
    text.resize(if n < 1024 { n } else { 1024 });
    return text;
}

// The kind gs_watch_next frames beside each path, as a Kind.
// The C layer frames 1/2/3 (see GS_WATCH_VERB_* in src/watcher_api.h);
// anything else is a layer bug, reported as false, and the event is
// dropped by each_event rather than delivered with a made-up kind.
fn kind_of(kind: i32) -> Kind, bool {
    if kind == 1 { return Kind.Created, true; }
    if kind == 2 { return Kind.Modified, true; }
    if kind == 3 { return Kind.Deleted, true; }
    return Kind.Created, false;
}

// Rendering only: "created", "modified", or "deleted". Compare kinds
// with match or ==, not with this string.
fn kind_name(kind: Kind) -> const u8[:] {
    match kind {
        Created => { return "created"; },
        Modified => { return "modified"; },
        Deleted => { return "deleted"; },
    }
}

// Starts watching the directory: the Watcher and "" on success, an empty
// Watcher and the reason on failure.
fn open(dir: const u8[:]) -> Watcher, u8[] {
    let h = open_handle(dir);
    if h.id == 0 {
        return Watcher { id: 0 }, error();
    }
    return h, "";
}

// F(ev) for each change, until F returns false or the watch fails.
// Blocks between bursts; Ctrl-C stops the program.
fn each_event<F>(w: Watcher&) {
    // One burst per gs_watch_next call: it blocks (-1: forever), gathers
    // a full burst inside its quiet window, and frames u32 path_len LE,
    // u32 kind LE, path bytes. A -2 (burst larger than the fixed drain)
    // ends the watch, as in v1: the C layer spends the burst either way,
    // so there is nothing to retry with.
    var buf: u8[65536];
    loop {
        let n = next_into(copy(w), buf[..], -1);
        if n == 0 { continue; }
        if n < 0 { return; }
        var off = 0;
        var count = 0;
        var keep = true;
        while count < n {
            let len = binary::read_u32le(buf[..], off) as! i64;
            let framed = binary::read_i32le(buf[..], off + 4);
            let path = buf[off + 8..off + 8 + len];
            let kind, ok = kind_of(framed);
            if ok && !F(Event { path: path, kind: kind }) { keep = false; break; }
            off = off + 8 + len;
            count = count + 1;
        }
        if !keep { return; }
    }
}

// Releases the watch. Idempotent: closing a closed watch does nothing.
fn close(w: Watcher&) {
    close_handle(copy(w));
    w.id = 0;
}
