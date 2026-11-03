# The watcher module (`watcher`)

This document describes the watcher package's implementation. The API is
the module source, `watcher.goose`: `open`, `each_event`, `close`, and
the kinds below.

## What it is

A program that declares the package (see README; bare `import watcher;` alone does not resolve yet) gets one-directory file watching:
`open(dir)` starts, `each_event` calls `F(ev)` for each change, where
`ev` is an `Event { path, kind }`
until `F` returns false, `close` releases. It is built the way `physics`
is, with the OS's own directory notifications in the place of Box3D:

    Goose program --extern fn--> gs_watch_* (goose_watcher, native C) --> inotify/kqueue/ReadDirectoryChangesW

The layer, `goose_watcher` (`src/`), is native C compiled once into a
static library: `goose run` loads it for JIT runs through the symbol
table the compiler generates from the module's `extern fn`s, and a
program built from the generated C links the same archive. Neither the
generated C nor TinyCC sees an OS header.

## The kinds

Each change reports a net change against the snapshot taken at `open`
and refreshed after every burst: `Created` (there now, not then),
`Deleted` (then, not now), `Modified` (there both times, with a content
write in between). Reads, opens, closes, and metadata-only touches
report nothing; paths that came and went inside one burst (editor temps)
report nothing. A rename reads as its two halves: the old path Deleted,
the new one Created. Subdirectories are not watched recursively, and
symlinks are not followed.

## The quiet window

Changes surface after a 100 ms quiet window, so a burst of writes still
reports one change per file. An atomic save (write temp, rename over)
completes inside the window and folds to one `Modified` on the real
file, with the temp silent — provided the save's own writes land in one
burst and its neighbors in others. A whole create-modify-delete
lifecycle inside one window folds to silence, since the net change is
nothing; that is the documented semantics, not a missed event.

## Framing

`gs_watch_next` copies one burst's events into the caller's slice as
`u32 path_len LE, u32 kind LE (1/2/3), path bytes` (framing values;
the public taxonomy is the Kind enum), and returns the
event count: 0 on timeout, -1 on fatal error, -2 with the required byte
count in `out[0..8]` when the slice is too small (the second call with
a larger slice gets the rest). The Goose side drains with a fixed
64 KiB slice; the C layer never grows a Goose stack.

## Thread safety

The layer is thread-safe by construction: per-watch state, a
mutex-guarded slot table with watcher generations (a closed id is an
error, not a crash), and per-thread error text. One watch must only be
used by one thread at a time; different watches may live on different
threads. There is no `main_thread_only`: a `thread_fn` may watch.

## Backends

One file, `src/watcher_poll.c`, behind one static vtable (`init`,
`wait`, `drain`, `close`): inotify + poll + read on Linux, kqueue with
`EVFILT_VNODE` on macOS, `ReadDirectoryChangesW` on an event on
Windows. Each is dependency-free (kernel32 only on Windows); the
snapshot diff resolves the kind on every platform. macOS and Windows
are marked experimental until platform CI runs them: the structure
(vtable + `#ifdef`) is the deliverable here, and only the Linux backend
has run.

## How it is built

A package (`docs/design/packages.md` in the Goose repository): this
repository holds the module `watcher.goose`, the sidecar
`watcher.build` naming the CMake target `goose_watcher`, the layer
under `src/`, and the CMakeLists that builds it with no dependencies.
`import watcher;` would find it through the blessed table
(`src/blessed.txt`) only once a watcher row lands there; today there is
no such row, so programs must declare the package explicitly (see
README). A compiler configured with `-DGOOSE_BUILTIN_LIBS=all` (or
`watcher`) would then have it compiled in (`cmake/package.cmake`): its
functions reach a JIT run through the symbol table CMake generates from
the module's `extern fn`s, and the link closure of a probe executable
is the response file `goose build` links programs with. A compiler
without a built-in copy builds the package into `GOOSE_HOME` the first
time a program declares it. `test/api_check.py`
checks the module and the header agree; it accepts both extern forms
(Spec §7.10), since this module uses the current `= "sym"` spelling.
