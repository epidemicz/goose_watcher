// Polls for changes instead of blocking on them: one event per call,
// then the loop carries on — the shape a main loop wants, with no
// thread needed.
//
//   ./hello.txt - create
//   ./hello.txt - write
//   ./hello.txt - chmod
//   ./hello.txt - remove
//
// Usage: goose run watch_directory_polling.goose -- [dir] (default: the
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
    var p = watcher::Poller { w: w, .. };
    print("polling '", dir, "' (100 ms per check), Ctrl-C to stop");
    loop {
        // The 100 ms wait paces the loop: a 0 here in a tight loop
        // with no other work WILL spin (see watcher.goose).
        match p.poll(100) {
            Received e => { print(e.ev); },
            Timeout => {},    // nothing yet; other work below carries on
            Error e => {
                // Overflow loses a burst with the watch alive; anything
                // else is fatal and the watch must be closed.
                match e.kind {
                    Overflow, QueueOverflow => { print("watch overflow: ", e.msg, " (continuing)"); },
                    Fatal => { print("watch ended: ", e.msg); w.close(); return; },
                }
            },
        }
        // Other work goes here.
    }
}
