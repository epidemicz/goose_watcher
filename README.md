# goose_watcher

Directory watching for the [Goose](https://github.com/aardappel/goose)
programming language, on the OS's own notifications: each change in one
directory as a `(name, verb)` pair, where the verb is `created`,
`modified` or `deleted` and describes the net change. A Goose *package*:
the module `watcher.goose`, its C layer under `src/`, and the
CMakeLists that builds the layer with no dependencies.

[docs/watcher.md](docs/watcher.md) describes how the layer is built.

## Using it

Watcher is one of Goose's blessed packages: a program imports it by name,

```goose
import watcher;
```

and the compiler knows where it comes from. A compiler built with
`-DGOOSE_BUILTIN_LIBS=all` has the package compiled in and runs such a
program with nothing else installed; otherwise the compiler fetches this
repository at the pinned tag into `~/.goose` the first time, builds the
layer with cmake and the C compiler it finds on the machine, and keeps it
for every program after that.

To use a particular version, a fork, or a checkout of your own, declare the
package in the program's root file instead:

```goose
import watcher from "owner/goose_watcher@v1";   // this repository, at a tag
import watcher from "../goose_watcher";          // a directory, used in place
```

`goose build program.goose` links the layer into the executable, and `goose
run program.goose` runs it in-process. The examples show it in use:

```goose
import watcher;

fn main() {
    var w, err = watcher::open(".");
    guard err.len == 0 else { print("watch: ", err); return; }
    w.each_change() { name, verb => print(name, " - ", verb); };
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
| `watcher.goose` | the module: the `Handle` struct that crosses to C, the `extern fn`s and thin wrappers |
| `watcher.build` | the package's sidecar: the CMake target to build, and that `examples/` is not the package's own Goose |
| `CMakeLists.txt` | builds `goose_watcher`, the layer, as a static library |
| `src/` | the layer: `watcher_api.h` lists every function the module declares; `test/api_check.py` checks the two agree |
| `examples/` | own-root programs: a threadless printer and a threaded consumer |

## License

Apache License 2.0, as Goose.
