# Changelog

## Unreleased (in development — not yet tagged)

Breaking rework of the whole API since the `v1` tag (`ec17e07`).
Nothing here is released; the `v1` tag below still carries the previous
API, and no new tag exists yet.

### What changed

- Multi-directory watching. `open(dir) -> Handle, u8[]` (one directory,
  error text as the second return) is gone. `create() -> Watcher, bool`
  starts a watcher with no paths; `add(path)` / `remove(path)` manage
  the watched set, each returning `bool` with the reason in `w.error()`;
  `watch_list()` returns the still-watched paths as `u8[][>..]`, sorted
  bytewise; `close` releases. `is_valid` reports whether a watcher is
  usable.
- Errors are trailing-`bool` + `error()`. The old `u8[]` error-text
  returns are gone everywhere: every fallible call returns `false` (or a
  zero handle plus `false`) and `w.error()` says why.
- Events are `Event { path, op, renamed_from }`. The old
  `each_change` callback took `(name, verb)` with `verb` a
  `"created"`/`"modified"`/`"deleted"` string naming a bare entry. The
  new taxonomy is the `Op` enum — `Create`, `Write`, `Remove`, `Rename`,
  `Chmod` — matched, never string-compared. `path` joins the watched
  dir and the entry name; `renamed_from` names the old path on the new
  path's `Create` half (Linux pairs them; elsewhere the halves read
  unpaired).
- `each_event` replaces `each_change`: `F(ev)` per change, `true` to
  keep watching, `false` to stop. It returns `bool` — `true` where `F`
  stopped it, `false` where the watch died fatally — so the two exits
  are distinguishable.
- Single-event polling via `Poller` / `poll` / `PollResult`.
  `var p = watcher::Poller { w: w, .. };` then `p.poll(timeout_ms)`
  returns one event per call: `Received { ev }`, `Timeout`, or
  `Error { kind, msg }`. `kind` is a `WatchError` — `Overflow` /
  `QueueOverflow` mean a lost burst with the watch alive (keep polling),
  `Fatal` means close and reopen — so callers match the enum instead of
  comparing error strings. `msg` is the printable reason. Borrowed
  payloads: `ev` and `msg` borrow the `Poller` until the next `poll`
  call that fetches a new burst.
- `print(ev)` renders events. `format` overloads give `Op` and `Event`
  text forms (`old -> new - create` for renames, `path - op`
  otherwise), so the renamed-from branch in user code collapses to one
  line.
- Resolver semantics: an atomic save (write temp, rename over) and a
  delete-then-recreate now report `Write` — they carry new contents —
  where the old layer reported `modified` on the save and silence or a
  split pair on delete-recreate. Create still subsumes a same-window
  write/chmod; write still dominates chmod; a create-modify-delete
  lifecycle inside one window still folds to silence.
- Framing grew a third length: `u32 path_len, u32 op, u32 renamed_len,
  path bytes, renamed bytes` per event (was `u32 name_len, u32 verb,
  name bytes`). The `OP_*` values are fsnotify's bit numbers
  (1/2/4/8/16); the public taxonomy stays a C-like enum, one op per
  event, never a mask.
- Overflow is classified, not silent-or-string. `each_event` still
  skips a lost burst silently and keeps watching; `poll` surfaces it
  as `Error` with `Overflow` (burst larger than the 64 KiB drain) or
  `QueueOverflow` (kernel `IN_Q_OVERFLOW`).
- `available()` is gone: linker presence implies availability, no probe.
- The module needs a Goose compiler newer than the v2026.3 release (see
  the README's Using section): the modern `extern fn = "sym"` form and
  `binary::read_i32le` postdate that release.
- Examples are blocking + polling only
  (`examples/watch_directory_blocking.goose`,
  `examples/watch_directory_polling.goose`). The old `print_changes`
  and `threaded_consumer` examples are gone (the threaded one remains
  in history).
- Tests: `python3 test/run_tests.py` runs `test/api_check.py`
  (module↔header parity), `goose check` on module + examples + tests,
  `test/watcher_tests.goose` behaviour tests, and a chmod probe through
  the polling example.

### Migrating from the v1 tag

```goose
// v1 tag: one directory, (name, verb) strings
var w, err = watcher::open(".");
guard err.len == 0 else { print("watch: ", err); return; }
w.each_change() { name, verb => print(name, " - ", verb); };

// Current: a watched set, typed events
var w, ok = watcher::create();
guard ok else { print("watch: ", w.error()); return; }
guard w.add(".") else { print("watch: ", w.error()); return; }
w.each_event() { ev => print(ev); true };
```

- `open(dir)` → `create()` + `add(dir)`. `Handle` → `Watcher`.
- `(name, verb)` strings → `Event { path, op, renamed_from }`:
  `"created"` → `Op.Create`, `"modified"` → `Op.Write`, `"deleted"` →
  `Op.Remove`, plus the new `Rename` (old path's half) and `Chmod`
  (metadata-only). `name` was a bare entry; `path` is the joined
  watched-dir + entry — pass an absolute path to `add` for absolute
  event paths.
- `err.len == 0` guards → `bool` guards with `w.error()` in the else
  branch.
- A rename used to read as delete + create halves; it now reads as
  `Rename` on the old path and `Create` with `renamed_from` on the new
  one. Match `ev.op`, and use `renamed_from` (or just `print(ev)`)
  instead of correlating halves by timing.
- For polling loops, use `Poller` + `poll` (see the polling example)
  instead of blocking in `each_event` on a thread.

## v1 (`ec17e07`) — the tagged previous API

Single-directory watching: `open(dir)` returns a `Handle`, `each_change`
calls `F(name, verb)` with a `"created"` / `"modified"` / `"deleted"`
string per change, `close` releases. Importable as
`import watcher from "owner/goose_watcher@v1";`.
