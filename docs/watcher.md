# The watcher module (`watcher`)

Normative for semantics. The API is the module source, `watcher.goose`.
Subdirectories are not watched recursively, and symlinks inside a
watched directory are not traversed: a symlinked entry is reported as
an entry, not watched as a directory of its own.

## Public surface

The API is `create`, `add`, `remove`, `watch_list`,
`each_event`, `poll`, `close`, `error`, `is_valid`, `Op`,
`Event`, `Poller`, `PollResult`, `WatchError`, and the `format`
overloads for `Op` and `Event` (so `print(ev)` and `print(ev.op)`
render directly). Everything else in `watcher.goose` — the `extern`
entry points, `OP_*` pins, `op_of`, the blob fetches, buffer sizes, and
overflow literals — is the FFI boundary, not for callers.

Failures return `false` (or a zero handle plus `false` from `create`),
and `w.error()` says why; `is_valid` reports whether the watcher is
usable. `poll` classifies errors into `WatchError` and carries the
message in the result: match `e.kind` for control flow, print `e.msg`
for the reason.

## What it is

A program that declares the package gets multi-directory file watching:
`create()` starts a watcher with no paths, `add(dir)` watches a
directory, `each_event` calls `F(ev)` for each change (and `poll` on a
`Poller` returns one event per call, for polling loops), where `ev`
is an `Event { path, op, renamed_from }`, until `F` returns false
(`each_event` then returns `true`; `false` means the watch died —
see Polling and errors); `remove(dir)` stops watching one path,
`watch_list()` lists the paths still watched, `close` releases:

    Goose program --extern fn--> gs_watch_* (goose_watcher, native C) --> inotify/kqueue/ReadDirectoryChangesW

The layer, `goose_watcher` (`src/`), is native C compiled once into a
static library. Use a checkout import (`import watcher from
"../goose_watcher";`) or declare the package in the program's root
file. Bare `import watcher;` only works in a compiler with this
package built in as a blessed package; watcher is not blessed
upstream, so a stock Goose compiler does not resolve it that way.

## The ops

Each change reports a net change against the snapshot taken at `add`
and refreshed after every burst, as one of fsnotify's five ops:

* `Create`: there now, not then (creation subsumes a same-window write
  or chmod — the file's appearance is the news).
* `Write`: there both times, with a content write in between (write
  dominates chmod on survivors). A survivor touched by a create or a
  remove in the window — a delete-then-recreate, or a move-in landing
  on an existing name — also reports `Write`: it carries new
  contents, so silence would be wrong.
* `Remove`: then, not now, with no rename observed.
* `Rename`: then, not now, with a rename observed (the old path's half).
* `Chmod`: there both times, with only a metadata change in between.

One op per event, matched never masked: the Goose taxonomy is a C-like
enum, not fsnotify's bitmask. Reads report nothing; paths that came and
went inside one burst (editor temps) report nothing. A rename reports
its two halves, old first: the old path `Rename`, then the new one
`Create` with `renamed_from` naming the old path. A `Write` from a
rename-over carries `renamed_from` naming the source file (the temp
file, for an atomic save); a direct `Write` has empty `renamed_from`.

`Event.path` joins the watched dir and the entry name: `add("dir")`
yields `dir/file`, `add("/abs/dir")` yields `/abs/dir/file`,
`add(".")` yields `./file`. Pass an absolute path to `add` for
absolute event paths. The path is otherwise used as given — not made
absolute, not canonicalized — except that trailing slashes are
stripped from the watched dir before joining, so `add("dir/")` also
yields `dir/file`, and `watch_list()` reports the stripped form.
`renamed_from` follows the same rule (the joined old name), and is
empty where no rename paired.

Rename pairing needs both halves visible: on Linux the inotify cookie
pairs a `MOVED_FROM` with its `MOVED_TO` (a 10-slot LRU, fsnotify's
`koekje`); a lone `MOVED_FROM` (moved out of every watched dir)
resolves `Rename` with empty `renamed_from`, a lone `MOVED_TO` resolves
`Create` with empty `renamed_from`. On macOS and Windows the halves
read unpaired: child renames report as `Remove`+`Create` (kqueue names
nothing per file) or pair within one packet only
(`ReadDirectoryChangesW` OLD→NEW halves).

## Adding, removing, listing

`add(path)` watches the directory: `true` on success, `false` on failure
with the reason in `w.error()`. Adding the same path twice is a no-op
returning `true`. Only directories can be watched: files and missing
paths fail with `"watcher: cannot watch '<path>': missing or not
a directory"`, and an empty path fails the same way (`"watcher: cannot
watch '': missing or not a directory"`). A symlink to a directory is
followed to its target — the watched set records the path you gave,
so events still report through that path, not the target's.

`remove(path)` stops watching the path: `true` on success, `false` where
the path was never added (the reason in `w.error()`: `"fsnotify: can't
remove non-existent watch: <path>"`). `watch_list()` returns the
user-added paths still watched, one finished string each, sorted
bytewise; empty means none, and a closed watcher yields `[]` too
(`is_valid` distinguishes).

A watch is automatically removed when its directory is deleted or
renamed: the watched dir itself reports one self event (`Remove` on
delete, `Rename` on rename) naming the dir, then drops out of the set
while the watcher stays open on the rest. Watching both a dir and its
parent reports the `Remove` twice (once per watch): the same
information fsnotify dedups, kept here as two events.

## The quiet window

Changes surface after a 100 ms quiet window, so a burst of writes still
reports one change per file. An atomic save (write temp, rename over)
completes inside the window and folds to one `Write` on the real
file, with the temp silent — the `Write` carries `renamed_from`
naming the temp file. A whole create-modify-delete lifecycle inside
one window folds to silence, since the net change is nothing. A
delete-then-recreate does not: it reports `Write` (see the ops above).

## Framing

`gs_watch_next` copies one burst's events into the caller's slice as
`u32 path_len LE, u32 op LE (1/2/4/8/16), u32 renamed_len LE, path
bytes, renamed bytes` (framing values pinned as `OP_*` in
`watcher.goose` against `GS_WATCH_OP_*`, fsnotify's bit numbers; the
public taxonomy is the Op enum, one single-bit op per event, never a
mask), and returns the event count: 0 on timeout, -1 on error
(fatal, or a kernel queue overflow the Goose side classifies),
-2 when the burst exceeds the slice. -2 spends the burst and advances
the snapshot; the Goose side surfaces it as a catchable `Error` the
watch continues from (fixed 64 KiB drain, the lost burst
unrecoverable but the watch alive). The required byte count lands in
`out[0..8]` only if `out.len >= 8`; if `out.len < 8` no size is
delivered. The Goose side drains with a fixed 64 KiB slice; the C
layer never grows a Goose stack.

## Rendering

`print(ev)` renders an event — `old -> new - create` for renames,
`path - op` otherwise — and `print(ev.op)` renders the op alone
(`create`, `write`, `remove`, `rename`, `chmod`), via `format`
overloads in the module. Compare ops with `match` or `==`, not with
the rendered text.

## Polling and errors

`poll(p, timeout_ms)` on a `Poller` (made as a local, `var p =
watcher::Poller { w: w, .. };`) reports one event per call:
`Received { ev }` where an event arrived, `Timeout` on timeout,
`Error { kind, msg }` on a poll-visible error (match `e.kind` for
control flow, print `e.msg` for the reason). The timeout applies only
when the Poller's pending burst is empty: queued events return at
once, with no wait. A burst that contains only unknown-framed events
reads as `Timeout`: those events are dropped, never delivered with a
made-up op, and the next poll fetches the next burst. After `Error`,
the watch continues unless the kind is fatal:

* `Overflow` (-2, `"watcher: burst larger than the drain"`): the burst
  was lost, the watch is alive — continue polling.
* `QueueOverflow` (kernel `IN_Q_OVERFLOW`, `"fsnotify: queue or buffer
  overflow"`): the queue lost events, the watch is alive — continue
  polling.
* `Fatal`: anything else — close the watch and reopen.

`each_event` never reports overflow: it skips a lost burst silently
and keeps watching; use `poll` to observe overflow. It is a loop over
`poll(-1)`, not a second delivery path. It returns `true` where the
block stopped it, `false` where the watch died fatally — the two
exits are otherwise indistinguishable, so check the return if it
matters.

`timeout_ms < 0` waits forever for one event, `= 0` returns
immediately if no event is ready, `> 0` waits up to that many
milliseconds for the next burst. A timeout of `0` in a tight loop
with no other work WILL spin: give the loop a sleep, a frame budget,
or a positive timeout.

## Borrowing

`Event.path` and `Event.renamed_from` are valid during the callback
only; from `poll`, they — and an `Error`'s `msg` — borrow the Poller
until the next poll call that fetches a new burst. Copy them (e.g.
into a fixed array) to cross a thread queue or to keep them longer.
This is enforced, not just advised: the compiler rejects a stored
payload used across a poll call that shrinks the Poller's buffer, so
a kept event must be copied before the next poll, not after it.

## Thread safety

The layer is thread-safe by construction: per-watch state, a
mutex-guarded slot table with watcher generations (a closed id is an
error, not a crash), and per-thread error text. One watch must only be
used by one thread at a time; different watches may live on different
threads.

## Backends

One file, `src/watcher_poll.c`, with one static add/wait/drain/close
set per platform: inotify + poll + read on Linux (mask
`IN_CREATE|IN_MODIFY|IN_ATTRIB|IN_DELETE|IN_DELETE_SELF|IN_MOVED_FROM|`
`IN_MOVED_TO|IN_MOVE_SELF|IN_Q_OVERFLOW|IN_IGNORED`, fsnotify's default
mask plus the two self events; cookie-LRU rename pairing), kqueue with
`EVFILT_VNODE` on macOS (NOTE→op mapping per fsnotify, no rename
pairing), `ReadDirectoryChangesW` on an event on Windows (action→op
mapping per fsnotify, single-packet OLD→NEW pairing, never `Chmod`).
Each is dependency-free; the snapshot diff resolves the op on every
platform. The Linux backend is tested. The Windows backend is tested
for the current API except for the two sections below; the macOS
backend is untested and experimental.

Windows limitations, both verified, not just absent from the tests:
attribute changes are not watched at all — the notify filter
excludes `FILE_NOTIFY_CHANGE_ATTRIBUTES`, so a chmod reports no
event, not a `Write` — and a burst created while the app is not
polling can lose events silently once notifications exceed the
kernel's 64 KiB queue. In testing, 1,000 files created at once
delivered all 1,000 events, while 1,500 created at once delivered
only the first event, with no error, and the watch stayed alive
afterwards. The drain-framing `Overflow` therefore only reports
bursts that are actually delivered to Goose (as Linux inotify
queueing reliably does, or as continuous polling does on Windows);
it is not a guarantee against loss in a create-then-poll burst on
Windows. `run_tests.py` skips the overflow section and the chmod
check on Windows for exactly these reasons.

## How it is built

This repository holds the module `watcher.goose`, the sidecar
`watcher.build` naming the CMake target `goose_watcher`, the layer
under `src/`, and the CMakeLists that builds it. `test/api_check.py`
checks the module and the header agree; behaviour is asserted in
`test/watcher_tests.goose`, run with the other tests by
`test/run_tests.py` (see the README's Tests section). The module needs
a Goose compiler newer than the v2026.3 release (see the README's
Using section for why).
