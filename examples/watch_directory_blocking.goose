// Prints one line per change in the directory:
//
//   ./hello.txt - create
//   ./hello.txt - write
//   ./hello.txt - chmod
//   ./hello.txt - remove
//
// Usage: goose run watch_directory_blocking.goose -- [dir] (default: the
// current directory). Ctrl-C stops it.
import watcher from "..";
import os;   // bare import: args()/exit() below resolve unqualified

fn main() {
    let argv = args();
    let dir = if argv.len > 1 { argv[1] } else { "." };
    var w, ok = watcher::create();
    guard ok else {
        print("watch: ", w.error());
        exit(1);
    }
    guard w.add(dir) else {
        print("watch: ", w.error());
        exit(1);
    }
    print("watching '", dir, "', Ctrl-C to stop");
    w.each_event() { ev => print(ev); true };
    // Reachable only where F returns false or the watch ends fatally:
    // not dead code, just past the blocking loop.
    w.close();
}
