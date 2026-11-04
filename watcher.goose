// Watches a set of directories and reports each change as an Event,
// where op is Create, Write, Remove, Rename, or Chmod and describes the
// net change. An atomic save (write temp, rename over) reports Write on
// the real file; short-lived temp files report nothing.
//
//   import watcher from "./watcher";
//   fn main() {
//       var w, ok = watcher::create();
//       guard ok else { print("watch: ", w.error()); return; }
//       guard w.add(".") else { print("watch: ", w.error()); return; }
//       w.each_event() { ev => print(ev); };
//   }
//
// The public surface is create, add, remove, watch_list,
// each_event, poll, close, error, is_valid, Op,
// Event, Poller, PollResult, WatchError, and the format overloads.
// Everything below the "internal" divider is the FFI boundary, not
// the API.
//
// Changes surface after a short quiet window (100ms), so a burst of
// writes still reports one change per file. A rename reports its two
// halves: the old path Rename, the new one Create with renamed_from
// naming the old path (Linux pairs them; elsewhere the halves read
// unpaired). Reads report nothing; subdirectories are not watched
// recursively. The callback returning false stops the watch cleanly.
//
// Event.path joins the watched dir and the entry name with no
// normalization: add("dir") yields dir/file, add("/abs/dir") yields
// /abs/dir/file, add(".") yields ./file. Pass an absolute path to add
// for absolute event paths. renamed_from follows the same rule. In a
// callback, both slices borrow the drain buffer for the callback only.
// From poll, both slices — and an Error's msg — borrow the Poller until
// the next poll call that fetches a new burst: use them at once, or
// copy them if they must outlive that (copy them, e.g. into a fixed
// array to cross a thread queue).
//
// The functions are C, the layer in src/ over the OS's own directory
// notifications (inotify on Linux, kqueue on macOS,
// ReadDirectoryChangesW on Windows), declared here as extern fns against
// the list in src/watcher_api.h; test/api_check.py checks the two agree.
// All blocking and all fd/handle lifetime stays in C; this file owns
// only the Goose-side pending burst plus a fixed drain slice. One watch
// must only be used by one thread at a time; different watches may live
// on different threads. A Poller must not be used after its watcher is
// closed.

namespace watcher;

import binary;
import std;   // sort()/compare() for the watch_list order

// --- ops: the public event taxonomy --------------------------------------
// fsnotify's five ops as a Goose enum: one op per event, matched never
// masked (Goose enums carry no bit ops, so the taxonomy is C-like, not a
// bitmask).

enum Op { Create, Write, Remove, Rename, Chmod }

// The op's text form: "create", "write", "remove", "rename", or "chmod".
// Compare ops with match or ==, not with this string.
fn format(out: u8[>..]&, op: Op) {
    match op {
        Create => { out.append("create"); },
        Write => { out.append("write"); },
        Remove => { out.append("remove"); },
        Rename => { out.append("rename"); },
        Chmod => { out.append("chmod"); },
    }
}

// --- structs: the types that cross to C, and the event -------------------
// Watcher is the C layer's slot id, 0 = none. Single-use: create a new
// one to restart. Don't build one by hand — use create, and is_valid to
// ask whether it is usable. Event is Goose-side only: it never crosses
// to C.

struct Watcher { id: i64 }

struct Event { path: u8[:], op: Op, renamed_from: u8[:] }

// A single-event poll in progress: the watcher, the pending burst's
// framed bytes, the offset into them, how many events are left in the
// burst, and the last error text. A root struct, because it owns two
// independently growing builders (buf and err) — an ordinary struct
// cannot hold two resizable fields in any order. The tradeoff is
// placement: a Poller must be a local or global variable, never a
// field or array element, and a root struct cannot be returned by
// value, so there is no poller() factory: construct one directly, as
// a local, with `var p = watcher::Poller { w: w, .. };`. It must be
// constructed at its declaration — `var p: Poller; p.w = w;` does not
// compile. Poller fields are the poll implementation's, not the
// caller's: make one, then poll() to read from it.
root struct Poller { w: Watcher, buf: u8[>..] = [], off: i64 = 0, left: i64 = 0, err: u8[>..] = [] }

// The event's text form: "old -> new - create" for renames, "path -
// write" otherwise. By reference: Event is not fixed-size, so it is
// never copied implicitly.
fn format(out: u8[>..]&, ev: Event&) {
    if ev.renamed_from.len > 0 { format(out, ev.renamed_from, " -> ", ev.path, " - ", ev.op); }
    else { format(out, ev.path, " - ", ev.op); }
}

// --- results: what the polling call reports ------------------------------

// What poll reports, one event at a time: an event arrived (Received
// carries it), nothing arrived before the timeout (Timeout), or a
// poll-visible error fired (Error carries its kind and its message).
// Match e.kind: Overflow (drain -2) and QueueOverflow (kernel
// IN_Q_OVERFLOW) mean a lost burst with the watch alive — continue
// polling; Fatal means anything else — close and reopen. Timeout also
// covers a burst that delivered nothing (every event dropped as
// unknown-framed): poll again. Received's ev and Error's msg borrow
// the Poller until the next poll call that fetches a new burst.
enum WatchError { Overflow, QueueOverflow, Fatal }
enum PollResult { Received { ev: Event }, Timeout, Error { kind: WatchError, msg: u8[:] } }

// --- the library ---------------------------------------------------------------

// Starts a watcher with no paths: the Watcher and true on success, an
// empty Watcher and false on failure, the reason in w.error(). Add
// directories with add.
fn create() -> Watcher, bool {
    let h = create_handle();
    if h.id == 0 {
        return Watcher { id: 0 }, false;
    }
    return h, true;
}

// Whether the watcher is usable: true after a successful create, false
// after close. Covers the "was create ok / did I close this" check
// without touching id directly.
fn is_valid(w: Watcher&) -> bool { w.id != 0 }

// Watches the directory: true on success, false on failure with the
// reason in w.error(). Adding the same path twice is a no-op returning
// true. Only directories can be watched: files, symlinks, and missing
// paths fail.
fn add(w: Watcher&, path: const u8[:]) -> bool {
    return add_path(copy(w), path);
}

// Stops watching the path: true on success (including after the path was
// auto-removed), false where the path was never added, the reason in
// w.error() ("fsnotify: can't remove non-existent watch: <path>").
fn remove(w: Watcher&, path: const u8[:]) -> bool {
    return remove_path(copy(w), path);
}

// The user-added paths still watched, one finished string each, sorted
// bytewise; empty means none. A path whose directory was deleted or
// renamed is auto-removed and no longer listed. A closed watcher yields
// [] too — is_valid distinguishes.
fn watch_list(w: Watcher&) -> u8[][>..] {
    let blob = list_blob(w);
    var views: const u8[:][>..] = [];
    var start = 0;
    for c, i in blob {
        if c == 0 {
            if i > start { views.push(blob[start..i]); }
            start = i + 1;
        }
    }
    sort(views[..]) { a, b => compare(a, b) < 0 };
    var out: u8[][>..] = [];
    for v in views { out.push(str(v)); }
    return out;
}

// Why the last call on w that failed did, as text. The receiver is
// ignored — the text is per-thread C state — but names whose failure it
// explains.
fn error(w: Watcher&) -> u8[>..] {
    return error_blob();
}

// One event, then returns — the polling delivery API, on a Poller
// made as a local: `var p = watcher::Poller { w: w, .. };`. Call it
// once per frame (or once per check) with a timeout, and it returns
// one event, a timeout with no event, or a poll-visible error.
//   timeout_ms < 0: wait forever for one event
//   timeout_ms = 0: return immediately if no event is ready
//   timeout_ms > 0: wait up to that many milliseconds for the next burst
// The timeout applies only when the pending burst is empty: events
// already queued return at once, with no wait. Received's ev and
// Error's msg borrow the Poller until the next poll call that fetches
// a new burst: use them at once, or copy them if they must outlive
// that — the compiler enforces this, rejecting a stored payload used
// across a poll call that shrinks the Poller's buffer. A timeout of 0
// in a tight loop with no other work WILL spin:
// give the loop a sleep, a frame budget, or a positive timeout (see
// the 100 ms wait in examples/watch_directory_polling.goose).
fn poll(p: Poller&, timeout_ms: i64) -> PollResult {
    var drain: u8[DRAIN];
    if p.left == 0 {
        // Fetch one burst: gs_watch_next gathers a full burst inside
        // its quiet window and frames u32 path_len LE, u32 op LE,
        // u32 renamed_len LE, path bytes, renamed bytes. 0 is a
        // timeout, -1 is an error, -2 spends a burst larger than the
        // fixed drain. Only valid-framed events are kept: unknown
        // framed ops are dropped here, never delivered with a made-up
        // op, and a burst that kept nothing reads as Timeout.
        let n = next_into(copy(p.w), drain[..], timeout_ms);
        if n == 0 { return PollResult.Timeout; }
        if n == -2 {
            p.err.resize(0);
            format(p.err, OVERFLOW_DRAIN);
            return PollResult.Error { kind: WatchError.Overflow, msg: p.err[..] };
        }
        if n == -1 {
            // Overflowed through the kernel queue, or fatal: classify
            // from the error text once, here, so the caller matches
            // e.kind and prints e.msg, never string-comparing.
            let why = error_blob();
            p.err = copy(why);
            if why == OVERFLOW_DRAIN { return PollResult.Error { kind: WatchError.Overflow, msg: p.err[..] }; }
            if why == OVERFLOW_QUEUE { return PollResult.Error { kind: WatchError.QueueOverflow, msg: p.err[..] }; }
            return PollResult.Error { kind: WatchError.Fatal, msg: p.err[..] };
        }
        p.buf.resize(0);
        p.off = 0;
        p.left = 0;
        var off = 0;
        var count = 0;
        // Read through a slice view: reading drain directly trips
        // the compiler's definite-assignment check, while the view —
        // the same form next_into filled — does not.
        let ds = drain[..];
        while count < n {
            let len = binary::read_u32le(ds, off) as! i64;
            let framed = binary::read_i32le(ds, off + 4);
            let rlen = binary::read_u32le(ds, off + 8) as! i64;
            if len < 0 || rlen < 0 || off + 12 + len + rlen > ds.len { break; }
            let op, ok = op_of(framed);
            if ok {
                format(p.buf, ds[off..off + 12 + len + rlen]);
                p.left += 1;
            }
            off += 12 + len + rlen;
            count += 1;
        }
        if p.left == 0 { return PollResult.Timeout; }
    }
    // Decode the next pending event out of the Poller's buffer.
    let len = binary::read_u32le(p.buf, p.off) as! i64;
    let framed = binary::read_i32le(p.buf, p.off + 4);
    let rlen = binary::read_u32le(p.buf, p.off + 8) as! i64;
    let path = p.buf[p.off + 12..p.off + 12 + len];
    let old = p.buf[p.off + 12 + len..p.off + 12 + len + rlen];
    let op, ok = op_of(framed);
    p.off += 12 + len + rlen;
    p.left -= 1;
    if !ok { return PollResult.Timeout; }
    return PollResult.Received { ev: Event { path: path, op: op, renamed_from: old } };
}

// F(ev) for each change, until F returns false or the watch fails.
// Return true from F to keep watching, false to stop cleanly. Blocks
// between bursts; Ctrl-C stops the program. each_event never reports
// overflow: a burst larger than the drain (or a kernel queue overflow)
// is skipped silently and the watch continues; use poll to observe
// overflow. Implemented as a loop over poll(-1): the simple blocking
// form over the same single-event core, not a second delivery path.
// Returns true where F stopped it, false where the watch died fatally
// — the two exits are otherwise indistinguishable.
fn each_event<F>(w: Watcher&) -> bool {
    var p = Poller { w: w, .. };
    loop {
        match p.poll(-1) {
            Received e => { if !F(e.ev) { return true; } },
            Timeout => { continue; },
            Error e => {
                match e.kind {
                    Overflow, QueueOverflow => { continue; },
                    Fatal => { return false; },
                }
            },
        }
    }
}

// Releases the watch. Idempotent: closing a closed watch does nothing.
fn close(w: Watcher&) {
    close_handle(copy(w));
    w.id = 0;
}

// --- internal: the FFI boundary, not the API ---
// Not for callers: the extern entry points, buffer sizes, framing pins,
// and blob fetches the public fns above stand on.

// The C entry points this file reaches through `extern "..." fn`, against
// the list in src/watcher_api.h; test/api_check.py checks the two agree.
// Linker presence implies availability: no `available()` probe exists.
extern fn create_handle() -> Watcher = "gs_watch_create";
extern fn add_path(h: Watcher, path: const u8[:]) -> bool = "gs_watch_add";
extern fn remove_path(h: Watcher, path: const u8[:]) -> bool = "gs_watch_remove";
extern fn list_into(h: Watcher, out: u8[:]) -> i64 = "gs_watch_watch_list";
extern fn next_into(h: Watcher, out: u8[:], timeout_ms: i64) -> i64 = "gs_watch_next";
extern fn close_handle(h: Watcher) = "gs_watch_close";
extern fn error_into(out: u8[:]) -> i64 = "gs_watch_error";

// The burst drain: 64 KiB. WATCH_PACKET is 64 KiB (Windows packet) and
// watch_state.drain is 16 KiB (Linux inotify read buffer); the sizes
// are intentionally not synchronized.
let DRAIN: i64 = 65536;

// The first-guess fetch size: the C layer owns a fixed 1024-byte error
// buffer but reports the full length, so both blob fetches start here
// and loop-resize on n > buf.len.
let ERRBUF: i64 = 1024;

// The values gs_watch_next frames beside each path, pinned against
// GS_WATCH_OP_* in src/watcher_api.h (test/api_check.py compares them).
// Single-bit only: C frames one op per event; the numbers match
// fsnotify's Op bits so the C drain reads like backend_inotify.go.
let OP_CREATE: i32 = 1;
let OP_WRITE: i32 = 2;
let OP_REMOVE: i32 = 4;
let OP_RENAME: i32 = 8;
let OP_CHMOD: i32 = 16;

// The two continuable reasons, in exactly one place: the classifier in
// poll compares against these, so callers never string-compare.
// String lets, never across C (api_check.py only pins i32/u8 lets).
let OVERFLOW_DRAIN = "watcher: burst larger than the drain";
let OVERFLOW_QUEUE = "fsnotify: queue or buffer overflow";

// The op gs_watch_next frames beside each path, as an Op.
// Anything else is a layer bug, reported as false, and the event is
// dropped by poll's refill walk rather than delivered with a made-up
// op. The Op.Create beside false is an unreachable sentinel: it
// is never delivered, because ok is false there.
fn op_of(framed: i32) -> Op, bool {
    if framed == OP_CREATE { return Op.Create, true; }
    if framed == OP_WRITE { return Op.Write, true; }
    if framed == OP_REMOVE { return Op.Remove, true; }
    if framed == OP_RENAME { return Op.Rename, true; }
    if framed == OP_CHMOD { return Op.Chmod, true; }
    return Op.Create, false;
}

// The shared loop-resize fetch for the NUL-joined list buffer; empty on
// a closed watcher (list_into returns -1 there).
fn list_blob(w: Watcher&) -> u8[>..] {
    var text: u8[>..] = [];
    text.resize(ERRBUF, 0);
    let n = list_into(copy(w), text[..]);
    if n > text.len {
        text.resize(n, 0);
        let m = list_into(copy(w), text[..]);
        text.resize(if m < n { m } else { n });
        return text;
    }
    if n < 0 {
        return [];
    }
    text.resize(n);
    return text;
}

// The shared loop-resize fetch for error text: C returns an int64_t
// length here; negative never occurs on this path.
fn error_blob() -> u8[>..] {
    var text: u8[>..] = [];
    text.resize(ERRBUF, 0);
    let n = error_into(text[..]);
    if n > text.len {
        text.resize(n, 0);
        let m = error_into(text[..]);
        text.resize(if m < n { m } else { n });
        return text;
    }
    text.resize(n);
    return text;
}
