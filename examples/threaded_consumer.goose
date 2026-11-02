// The threaded consumer: one worker prints what the watcher reports.
//
// Usage: goose build threaded_consumer.goose && ./threaded_consumer [dir].
// AOT only: JIT + threads is a known compiler limitation, unrelated to
// this package.
import watcher from "..";
import os;
import std;

struct FW {
    name: u8[..255],
    verb: u8[..16],
}

fn format(out: u8[>..]&, v: FW) {
    format(out, v.name, ": ", v.verb);
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
    w.each_change() { name, verb =>
        qput(FW { name: name, verb: verb });
        true
    };
    w.close();
}
