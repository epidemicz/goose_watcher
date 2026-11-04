# goose_watcher

Directory watching for the [Goose](https://github.com/aardappel/goose)
programming language, on the OS's own notifications: each change in a
set of directories as an `Event { path, op, renamed_from }`, where op is
`Create`, `Write`, `Remove`, `Rename`, or `Chmod` and describes the net
change. `path` joins the watched dir and the entry name. Subdirectories
are not watched recursively, and symlinks are not followed. A Goose
*package*: the module `watcher.goose`, its C layer under `src/`, and the
CMakeLists that builds the layer with no dependencies.

Read it in two ways: `each_event` blocks and calls your block for each
change, or `poll` on a `Poller` returns one event per call for a
polling loop. [docs/watcher.md](docs/watcher.md) describes the semantics.

## Using it

Import a checkout, used in place. The import path is relative to
your program's file: the path below means a checkout sitting
next to your program, not inside this repository.

```goose
import watcher from "../goose_watcher";   // a checkout
```

and the compiler builds the layer with cmake and the C compiler it
finds on the machine, and keeps it for every program after that.

The current API needs a Goose compiler newer than the v2026.3 release:
a build from main after Oct 31 2026. The v2026.3 release cannot parse
the module: it predates the modern `extern fn name(...) = "sym"` form
and `binary::read_i32le`, which the decode core uses. If `goose check`
or `goose run` fails on `watcher.goose` with a v2026.3 compiler, that
is why — build Goose main from source
(`cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`)
and use `build/goose`.

To use a particular version, a fork, or a checkout of your own, declare the
package in the program's root file instead:

```goose
import watcher from "../goose_watcher";          // a directory, used in place
import watcher from "epidemicz/goose_watcher@v1";   // v1
```

`goose build program.goose` links the layer into the executable, and `goose
run program.goose` runs it in-process.

Blocking, one line per change (see
`examples/watch_directory_blocking.goose`):

```goose
import watcher from "../goose_watcher";   // a checkout

fn main() {
    var w, ok = watcher::create();
    guard ok else { print("watch: ", w.error()); return; }
    guard w.add(".") else { print("watch: ", w.error()); return; }
    w.each_event() { ev => print(ev); true };
}
```

Polling, one event per call (see
`examples/watch_directory_polling.goose`):

```goose
import watcher from "../goose_watcher";   // a checkout

fn main() {
    var w, ok = watcher::create();
    guard ok else { print("watch: ", w.error()); return; }
    guard w.add(".") else { print("watch: ", w.error()); return; }
    var p = watcher::Poller { w: w, .. };
    loop {
        match p.poll(100) {
            Received e => { print(e.ev); },
            Timeout => {},
            Error e => {
                match e.kind {
                    Overflow, QueueOverflow => { print("watch overflow: ", e.msg, " (continuing)"); },
                    Fatal => { print("watch ended: ", e.msg); w.close(); return; },
                }
            },
        }
    }
}
```

The examples, run from this repository's root:

```sh
goose run examples/watch_directory_blocking.goose -- [dir]
goose run examples/watch_directory_polling.goose -- [dir]
```

The directory examples default to the current directory; pass a scratch
directory, not the repository root, unless you want to watch the
repository itself.

## Semantics to know before porting from fsnotify

This is close to fsnotify in function, not identical to it:

- Events are net changes after a 100 ms quiet window, one op per file
  per burst — not fsnotify's raw events. A burst of writes folds to one
  op, create subsumes a same-window write/chmod, write dominates chmod,
  and a create-modify-delete lifecycle inside one window folds to
  silence. A delete-then-recreate, and an atomic save (write temp,
  rename over), report `Write` instead — they carry new contents.
  That is the documented semantics, not a missed event.
- `add` watches directories only, not files. Rename pairing is
  Linux-only reliable (inotify cookie); macOS and Windows halves read
  unpaired. A rename delivers old first: the old path `Rename`, then
  the new path `Create` with `renamed_from` naming the old path.
  Watching both a dir and its parent reports a watched dir's
  self `Remove` twice, where fsnotify dedups.
- Overflow is skipped silently by `each_event` and surfaced by `poll`
  as `Error` with kind `Overflow` or `QueueOverflow`; anything else
  is `Fatal` — close and reopen. A burst that delivered nothing reads
  as `Timeout`: poll again.
- The Linux backend is tested; Windows is tested for the current
  API except for two known limitations, documented in
  `docs/watcher.md`: attribute changes report no event, and a burst
  created while the app is not polling can lose events silently
  once notifications exceed the kernel's 64 KiB queue. macOS is
  untested.

## Tests

Run every test with one command, from this repository's root:

```sh
python3 test/run_tests.py
```

That runs `test/api_check.py` (module against C header, see below),
`goose check` on the module, the examples, and the behaviour tests,
`test/watcher_tests.goose` (asserting behaviour: create/write/rename/
remove, atomic save, delete-recreate, silence, multi-file bursts,
timeouts, `each_event`'s return, overflow and watch-after-overflow),
and a chmod check driven through the polling example (Goose has no
chmod in its standard library, so that one goes through the shell).
On Windows, `run_tests.py` skips the overflow section and the chmod
check — see `docs/watcher.md` for why those two are not valid
expectations there; everything else runs and passes.
Each step also runs alone: `python3 test/api_check.py`,
`goose check watcher.goose`, `goose run test/watcher_tests.goose`.

## License

Apache License 2.0, as Goose.
