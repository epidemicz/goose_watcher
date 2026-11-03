# goose_watcher

Directory watching for the [Goose](https://github.com/aardappel/goose)
programming language, on the OS's own notifications: each change in one
directory as an `Event { path, kind }`, where kind is `Created`,
`Modified` or `Deleted` and describes the net change. `path` is relative
to the watched root. A Goose *package*:
the module `watcher.goose`, its C layer under `src/`, and the
CMakeLists that builds the layer with no dependencies.

[docs/watcher.md](docs/watcher.md) describes how the layer is built.

## Using it

Watcher is NOT a blessed package yet: Goose's `src/blessed.txt` today
lists only physics, gfx, audio, and ui, and has no watcher row, so a bare
`import watcher;` does not resolve on its own. The package is in
blessed-ready form, so that bare import will work once a watcher row
lands upstream. Until then, use a checkout of this branch, as below. Note the v1 tag
is the previous API (each_change with name/verb), not the Event/Kind
API described here; no released tag carries the new API yet.

```goose
import watcher from "../goose_watcher";   // a checkout, used in place
```

and the compiler builds the layer with cmake and the C compiler it
finds on the machine, and keeps it for every program after that. Once
blessed, a compiler built with `-DGOOSE_BUILTIN_LIBS=all` would have the
package compiled in and run a bare-import program with nothing else
installed; a released tag would then also be fetchable into `~/.goose`.

To use a particular version, a fork, or a checkout of your own, declare the
package in the program's root file instead:

```goose
import watcher from "epidemicz/goose_watcher@v1";   // v1, released, previous API
import watcher from "../goose_watcher";          // a directory, used in place
```

`goose build program.goose` links the layer into the executable, and `goose
run program.goose` runs it in-process. The examples show it in use:

```goose
import watcher from "../goose_watcher";   // a checkout; bare import only once blessed

fn main() {
    var w, err = watcher::open(".");
    guard err.len == 0 else { print("watch: ", err); return; }
    w.each_event() { ev => print(ev.path, " - ", watcher::kind_name(ev.kind)); };
}
```

## Platforms

| | backend | status |
|---|---|---|
| Linux | inotify + poll + read | tested |
| macOS | kqueue, `EVFILT_VNODE` | experimental |
| Windows | `ReadDirectoryChangesW` on an event | experimental |

## Layout

| | |
|---|---|
| `watcher.goose` | the module: the `Watcher` struct that crosses to C, the `Event`/`Kind`
public API, the `extern fn`s and thin wrappers |
| `watcher.build` | the package's sidecar: the CMake target to build, and that `examples/` is not the package's own Goose |
| `CMakeLists.txt` | builds `goose_watcher`, the layer, as a static library |
| `src/` | the layer: `watcher_api.h` lists every function the module declares; `test/api_check.py` checks the two agree |
| `examples/` | own-root programs: a threadless printer (threaded example paused) |

## License

Apache License 2.0, as Goose.
