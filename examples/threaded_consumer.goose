// The threaded consumer: one worker prints what the watcher reports.
//
// Usage: goose build threaded_consumer.goose && ./threaded_consumer [dir].
// AOT only: JIT + threads is a known compiler limitation, unrelated to
// this package.
import watcher from "..";
import os;
import std;

struct FW {
    path: u8[..255],
    kind: watcher::Kind,
}

fn format(out: u8[>..]&, v: FW) {
    format(out, v.path, ": ", watcher::kind_name(v.kind));
}

thread_fn handle_callback() {
    loop {
        let data = qget<FW>();
        print(data);
    }
}

fn main() {
    thread_spawn(handle_callback);
    let argv = args();
    let dir = if argv.len > 1 { argv[1] } else { "." };

    var w, err = watcher::open(dir);
    guard err.len == 0 else {
        print("watch: ", err);
        exit(1);
    }
    print("watching '", dir, "', Ctrl-C to stop");
    w.each_event() { ev =>
        qput(FW { path: ev.path, kind: ev.kind });
        true
    };
    w.close();
}
