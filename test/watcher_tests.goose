// Behaviour tests for the watcher, as opposed to test/api_check.py,
// which only checks that the module and the C header agree on
// signatures. These run the watcher and assert what it reports.
//
// Usage, from the repository root: goose run test/watcher_tests.goose
// (or python3 test/run_tests.py, which runs this plus api_check and
// the chmod check). Creates watcher_test_scratch and
// watcher_test_scratch2 in the current directory and removes them at
// the end, even on failure. Exit 0 when every check passes, 1 when
// any fails. The overflow check is the slow part (6,000 files in one
// burst, then deleting them again); everything else is one burst each.
//
// A scratch directory left by an interrupted run fails fast at the
// start rather than testing against files it did not create.
import watcher from "..";
import os;   // bare import: file/dir/sleep/exit below resolve unqualified

fn expect(cond: bool, label: const u8[:]) -> bool {
    if !cond { print("FAIL: ", label); return false; }
    print("ok: ", label);
    return true;
}

// The op as text, so checks read as create/write/... rather than
// enum comparisons.
fn op_name(ev: watcher::Event&) -> const u8[:] {
    match ev.op {
        Create => { return "create"; },
        Write => { return "write"; },
        Remove => { return "remove"; },
        Rename => { return "rename"; },
        Chmod => { return "chmod"; },
    }
}

fn main() {
    var fails = 0;
    let dir = "watcher_test_scratch";
    let dir2 = "watcher_test_scratch2";
    if is_dir(dir) || is_dir(dir2) {
        print("FAIL: scratch dir already exists; remove ", dir, " and ", dir2, " first");
        exit(1);
    }
    make_dir(dir);
    make_dir(dir2);

    var w, ok = watcher::create();
    if !expect(ok, "create succeeds") { fails += 1; }
    if !expect(w.is_valid(), "new watcher is valid") { fails += 1; }
    if !expect(!w.add("watcher_test_scratch_missing"), "add of a missing dir fails") { fails += 1; }
    if !expect(w.error().len > 0, "failed add leaves an error") { fails += 1; }
    if !expect(w.add(dir), "add scratch dir") { fails += 1; }
    if !expect(w.add(dir), "add same dir twice is a no-op success") { fails += 1; }
    if !expect(w.add(dir2), "add second dir") { fails += 1; }
    // watch_list's elements are variable-size, so it can only be
    // iterated, not indexed: order is checked by position in the loop.
    if !expect(w.watch_list().len == 2, "watch_list has both dirs") { fails += 1; }
    var wl_n = 0;
    var wl_first_ok = false;
    var wl_second_ok = false;
    for p in w.watch_list() {
        if wl_n == 0 && p == dir { wl_first_ok = true; }
        if wl_n == 1 && p == dir2 { wl_second_ok = true; }
        wl_n += 1;
    }
    if !expect(wl_first_ok, "watch_list is sorted, first is scratch") { fails += 1; }
    if !expect(wl_second_ok, "watch_list second is scratch2") { fails += 1; }
    if !expect(!w.remove("watcher_test_scratch_missing"), "remove of a never-added dir fails") { fails += 1; }
    if !expect(w.remove(dir2), "remove second dir") { fails += 1; }
    if !expect(w.watch_list().len == 1, "watch_list has one dir after remove") { fails += 1; }
    var wl_after_ok = false;
    for p in w.watch_list() {
        if p == dir { wl_after_ok = true; }
    }
    if !expect(wl_after_ok, "watch_list keeps the scratch dir") { fails += 1; }

    var p = watcher::Poller { w: w, .. };
    let f1 = "watcher_test_scratch/f1.txt";
    let f2 = "watcher_test_scratch/f2.txt";
    let real = "watcher_test_scratch/real.txt";
    let temp = "watcher_test_scratch/.tmp1";

    // Create, then a content write in a later burst.
    write_file(f1, "hello");
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == f1, "create reports the file") { fails += 1; }
            if !expect(op_name(e.ev) == "create", "create reports Create") { fails += 1; }
            if !expect(e.ev.renamed_from.len == 0, "create has no renamed_from") { fails += 1; }
        },
        Timeout => { print("FAIL: create timed out"); fails += 1; },
        Error e => { print("FAIL: create error ", e.msg); fails += 1; },
    }
    append_file(f1, " more");
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == f1, "write reports the file") { fails += 1; }
            if !expect(op_name(e.ev) == "write", "write reports Write") { fails += 1; }
        },
        Timeout => { print("FAIL: write timed out"); fails += 1; },
        Error e => { print("FAIL: write error ", e.msg); fails += 1; },
    }

    // Rename: old half first, then the new path with renamed_from.
    rename_file(f1, f2);
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == f1, "rename old half reports the old path") { fails += 1; }
            if !expect(op_name(e.ev) == "rename", "rename old half reports Rename") { fails += 1; }
        },
        Timeout => { print("FAIL: rename old half timed out"); fails += 1; },
        Error e => { print("FAIL: rename old half error ", e.msg); fails += 1; },
    }
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == f2, "rename new half reports the new path") { fails += 1; }
            if !expect(op_name(e.ev) == "create", "rename new half reports Create") { fails += 1; }
            if !expect(e.ev.renamed_from == f1, "rename new half names the old path") { fails += 1; }
        },
        Timeout => { print("FAIL: rename new half timed out"); fails += 1; },
        Error e => { print("FAIL: rename new half error ", e.msg); fails += 1; },
    }
    delete_file(f2);
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == f2, "remove reports the file") { fails += 1; }
            if !expect(op_name(e.ev) == "remove", "remove reports Remove") { fails += 1; }
        },
        Timeout => { print("FAIL: remove timed out"); fails += 1; },
        Error e => { print("FAIL: remove error ", e.msg); fails += 1; },
    }

    // Create-modify-delete inside one burst folds to silence.
    write_file("watcher_test_scratch/temp.txt", "x");
    delete_file("watcher_test_scratch/temp.txt");
    match p.poll(2000) {
        Received e => { print("FAIL: silence test got ", e.ev); fails += 1; },
        Timeout => { print("ok: create-delete in one burst is silent"); },
        Error e => { print("FAIL: silence test error ", e.msg); fails += 1; },
    }

    // Atomic save: a temp renamed over an existing file reports Write
    // on the real file, with renamed_from naming the temp.
    write_file(real, "old");
    match p.poll(2000) {
        Received e => {
            if !expect(op_name(e.ev) == "create", "real file creation reports Create") { fails += 1; }
        },
        Timeout => { print("FAIL: real file creation timed out"); fails += 1; },
        Error e => { print("FAIL: real file creation error ", e.msg); fails += 1; },
    }
    write_file(temp, "new");
    rename_file(temp, real);
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == real, "atomic save reports the real file") { fails += 1; }
            if !expect(op_name(e.ev) == "write", "atomic save reports Write") { fails += 1; }
            if !expect(e.ev.renamed_from == temp, "atomic save names the temp file") { fails += 1; }
        },
        Timeout => { print("FAIL: atomic save timed out"); fails += 1; },
        Error e => { print("FAIL: atomic save error ", e.msg); fails += 1; },
    }

    // Delete-then-recreate in one burst reports Write, no renamed_from.
    delete_file(real);
    write_file(real, "newer");
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == real, "delete-recreate reports the file") { fails += 1; }
            if !expect(op_name(e.ev) == "write", "delete-recreate reports Write") { fails += 1; }
            if !expect(e.ev.renamed_from.len == 0, "delete-recreate has no renamed_from") { fails += 1; }
        },
        Timeout => { print("FAIL: delete-recreate timed out"); fails += 1; },
        Error e => { print("FAIL: delete-recreate error ", e.msg); fails += 1; },
    }

    // Three files in one burst drain one event per poll, in any order.
    write_file("watcher_test_scratch/a.txt", "a");
    write_file("watcher_test_scratch/b.txt", "b");
    write_file("watcher_test_scratch/c.txt", "c");
    var seen_a = false;
    var seen_b = false;
    var seen_c = false;
    match p.poll(2000) {
        Received e => {
            if e.ev.path == "watcher_test_scratch/a.txt" { seen_a = true; }
            else if e.ev.path == "watcher_test_scratch/b.txt" { seen_b = true; }
            else if e.ev.path == "watcher_test_scratch/c.txt" { seen_c = true; }
            else { print("FAIL: burst event for unexpected path ", e.ev.path); fails += 1; }
            if !expect(op_name(e.ev) == "create", "burst event 1 reports Create") { fails += 1; }
        },
        Timeout => { print("FAIL: burst event 1 timed out"); fails += 1; },
        Error e => { print("FAIL: burst event 1 error ", e.msg); fails += 1; },
    }
    match p.poll(2000) {
        Received e => {
            if e.ev.path == "watcher_test_scratch/a.txt" { seen_a = true; }
            else if e.ev.path == "watcher_test_scratch/b.txt" { seen_b = true; }
            else if e.ev.path == "watcher_test_scratch/c.txt" { seen_c = true; }
            else { print("FAIL: burst event for unexpected path ", e.ev.path); fails += 1; }
            if !expect(op_name(e.ev) == "create", "burst event 2 reports Create") { fails += 1; }
        },
        Timeout => { print("FAIL: burst event 2 timed out"); fails += 1; },
        Error e => { print("FAIL: burst event 2 error ", e.msg); fails += 1; },
    }
    match p.poll(2000) {
        Received e => {
            if e.ev.path == "watcher_test_scratch/a.txt" { seen_a = true; }
            else if e.ev.path == "watcher_test_scratch/b.txt" { seen_b = true; }
            else if e.ev.path == "watcher_test_scratch/c.txt" { seen_c = true; }
            else { print("FAIL: burst event for unexpected path ", e.ev.path); fails += 1; }
            if !expect(op_name(e.ev) == "create", "burst event 3 reports Create") { fails += 1; }
        },
        Timeout => { print("FAIL: burst event 3 timed out"); fails += 1; },
        Error e => { print("FAIL: burst event 3 error ", e.msg); fails += 1; },
    }
    if !expect(seen_a && seen_b && seen_c, "burst delivered all three files") { fails += 1; }

    // Timeouts: an immediate check and a short wait, both with nothing pending.
    match p.poll(0) {
        Timeout => { print("ok: poll(0) with nothing pending is Timeout"); },
        Received e => { print("FAIL: poll(0) got unexpected ", e.ev); fails += 1; },
        Error e => { print("FAIL: poll(0) error ", e.msg); fails += 1; },
    }
    match p.poll(200) {
        Timeout => { print("ok: poll(200) with nothing pending is Timeout"); },
        Received e => { print("FAIL: poll(200) got unexpected ", e.ev); fails += 1; },
        Error e => { print("FAIL: poll(200) error ", e.msg); fails += 1; },
    }

    // each_event returns true where the block stops it. The file is
    // created before the call, so the blocking loop has an event to
    // stop on and cannot hang.
    write_file("watcher_test_scratch/each.txt", "e");
    let stopped = w.each_event() { ev => print("  each_event saw ", ev); false };
    if !expect(stopped, "each_event returns true when the block stops it") { fails += 1; }

    // Overflow: 6,000 files in one burst exceed the 64 KiB drain. The
    // watch stays alive afterwards: a later file still reports.
    for i in 0..6000 {
        var name: u8[>..] = [];
        format(name, "watcher_test_scratch/ov_", i, ".txt");
        write_file(name[..], "o");
    }
    match p.poll(5000) {
        Received e => { print("FAIL: overflow burst delivered an event ", e.ev); fails += 1; },
        Timeout => { print("FAIL: overflow burst timed out"); fails += 1; },
        Error e => {
            match e.kind {
                Overflow => { print("ok: overflow burst reports Overflow"); },
                _ => { print("FAIL: overflow burst reports wrong kind"); fails += 1; },
            }
            if !expect(e.msg.len > 0, "overflow error carries a message") { fails += 1; }
        },
    }
    match p.poll(500) {
        Timeout => { print("ok: after overflow, no pending events"); },
        Received e => { print("FAIL: after overflow, unexpected ", e.ev); fails += 1; },
        Error e => { print("FAIL: after overflow, error ", e.msg); fails += 1; },
    }
    write_file("watcher_test_scratch/after.txt", "a");
    match p.poll(2000) {
        Received e => {
            if !expect(e.ev.path == "watcher_test_scratch/after.txt", "watch alive after overflow reports the file") { fails += 1; }
            if !expect(op_name(e.ev) == "create", "watch alive after overflow reports Create") { fails += 1; }
        },
        Timeout => { print("FAIL: watch alive after overflow timed out"); fails += 1; },
        Error e => { print("FAIL: watch alive after overflow error ", e.msg); fails += 1; },
    }

    // Cleanup, even on failure, so a rerun starts clean.
    w.close();
    if !expect(!w.is_valid(), "closed watcher is not valid") { fails += 1; }
    delete_file("watcher_test_scratch/real.txt");
    delete_file("watcher_test_scratch/a.txt");
    delete_file("watcher_test_scratch/b.txt");
    delete_file("watcher_test_scratch/c.txt");
    delete_file("watcher_test_scratch/each.txt");
    delete_file("watcher_test_scratch/after.txt");
    for i in 0..6000 {
        var name: u8[>..] = [];
        format(name, "watcher_test_scratch/ov_", i, ".txt");
        delete_file(name[..]);
    }
    delete_dir(dir);
    delete_dir(dir2);
    
    guard fails == 0 else {
        print(fails, " test(s) failed");
        exit(1);
    }
    print("all watcher tests passed");
}
