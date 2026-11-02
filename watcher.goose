// Watches one directory and reports each change as (name, verb), where
// the verb is "created", "modified", or "deleted" and describes the net
// change. An atomic save (write temp, rename over) reports `modified`
// on the real file; short-lived temp files report nothing.
//
//   import watcher from "./watcher";
//   fn main() {
//       var w, err = watcher::open(".");
//       guard err.len == 0 else { print("watch: ", err); return; }
//       w.each_change() { name, verb => print(name, " - ", verb); };
//   }
//
// Changes surface after a short quiet window (100ms), so a burst of
// writes still reports one change per file. A rename reads as its two
// halves: the old name was deleted, the new one created. Reads and
// metadata-only touches report nothing; subdirectories are not watched
// recursively. The callback returning false stops the watch cleanly.
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

// --- constants: GS_WATCH_CONSTANTS in src/watcher_api.h -----------------------

// What gs_watch_next frames beside each name: 1 created, 2 modified,
// 3 deleted.
let VERB_CREATED: i32 = 1;
let VERB_MODIFIED: i32 = 2;
let VERB_DELETED: i32 = 3;

// --- structs: the packed C types in src/watcher_api.h --------------------------

// A live watch: the C layer's slot id, 0 = none. Single-use: open a new
// one to restart.
struct Handle { id: i64 }

// --- the library ---------------------------------------------------------------

// Whether the watcher layer is linked in. A compiler without it refuses to
// run a watcher program in-process, so this is true wherever it runs.
extern fn available() -> bool = "gs_watch_available";
extern fn open_handle(dir: const u8[:]) -> Handle = "gs_watch_open";
extern fn next_into(h: Handle, out: u8[:], timeout_ms: i64) -> i64 = "gs_watch_next";
extern fn close_handle(h: Handle) = "gs_watch_close";
extern fn error_into(out: u8[:]) -> i64 = "gs_watch_error";

// Why the last call that failed did, as text.
fn error() -> u8[>..] {
    var text: u8[>..] = [];
    text.resize(1024, 0);
    let n = error_into(text[..]);
    text.resize(if n < 1024 { n } else { 1024 });
    return text;
}

// The verbs gs_watch_next frames, as text. C frames only 1/2/3; anything
// else is a layer bug, reported as "".
fn verb_of(verb: i32) -> const u8[:] {
    if verb == VERB_CREATED { return "created"; }
    if verb == VERB_MODIFIED { return "modified"; }
    if verb == VERB_DELETED { return "deleted"; }
    return "";
}

// Starts watching the directory: the Watch and "" on success, an empty
// Watch and the reason on failure.
fn open(dir: const u8[:]) -> Handle, u8[] {
    let h = open_handle(dir);
    if h.id == 0 {
        return Handle { id: 0 }, error();
    }
    return h, "";
}

// F(name, verb) for each change, until F returns false or the watch
// fails. Blocks between bursts; Ctrl-C stops the program.
fn each_change<F>(w: Handle&) {
    // One burst per gs_watch_next call: it blocks (-1: forever), gathers
    // a full burst inside its quiet window, and frames u32 name_len LE,
    // u32 verb LE, name bytes. A -2 grows the drain and retries; the
    // burst waits in C.
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
            let verb = binary::read_i32le(buf[..], off + 4);
            let name = buf[off + 8..off + 8 + len];
            if verb_of(verb).len > 0 && !F(name, verb_of(verb)) { keep = false; break; }
            off = off + 8 + len;
            count = count + 1;
        }
        if !keep { return; }
    }
}

// Releases the watch. Idempotent: closing a closed watch does nothing.
fn close(w: Handle&) {
    close_handle(copy(w));
    w.id = 0;
}
