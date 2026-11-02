// Prints one line per change in the directory:
//
//   hello.txt - created
//   hello.txt - modified
//   hello.txt - deleted
//
// Usage: goose run print_changes.goose -- [dir] (default: the current
// directory). Ctrl-C stops it.
import watcher from "..";
import os;

fn main() {
    let argv = args();
    let dir = if argv.len > 1 { argv[1] } else { "." };

    var w, err = watcher::open(dir);
    guard err.len == 0 else {
        print("watch: ", err);
        exit(1);
    }
    print("watching '", dir, "', Ctrl-C to stop");
    w.each_change() { name, verb =>
        print(name, " - ", verb);
        true
    };
    w.close();
}
