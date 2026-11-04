#!/usr/bin/env python3
"""Runs every watcher test, from this repository's root:

    python3 test/run_tests.py

Steps, in order, each reported separately so one failure does not hide
the others: api_check (module against C header), goose check on the
module, the examples, and the behaviour tests, the behaviour tests
themselves, and a chmod check driven through the polling example (Goose
has no chmod in its standard library, so that one goes through the
shell). Scratch directories are removed before and after, even on
failure. Exit 0 when every step passes, 1 otherwise.

Two steps are platform-gated, both for Windows backend reasons, not
test bugs in the watcher logic itself: the behaviour tests skip their
overflow section on Windows (a burst created while the app is not
polling can lose events silently once notifications exceed the
kernel's 64 KiB queue, so drain-framing Overflow is not a valid
expectation there), and the chmod check is skipped on Windows (the
backend's notify filter excludes file attributes, so a chmod reports
no event at all, and Python's chmod 644->600 changes no Windows
attribute anyway). Output reading uses a reader thread, not select,
because select on Windows only works on sockets, not pipes.
"""

import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCRATCH = REPO / "watcher_test_scratch"
SCRATCH2 = REPO / "watcher_test_scratch2"
CHMOD_DIR = REPO / "watcher_chmod_scratch"


def run(cmd, cwd=REPO, timeout=300):
    print(f"$ {' '.join(cmd)}")
    try:
        proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        print("FAIL: timed out")
        return False
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        print(f"FAIL: exit {proc.returncode}")
        return False
    print("ok")
    return True


def clean():
    for d in (SCRATCH, SCRATCH2, CHMOD_DIR):
        shutil.rmtree(d, ignore_errors=True)


class ProcLines:
    """Lines from a process, read by a thread so no select is needed.

    select on Windows only works on sockets, not pipes, and fails
    outright there; a reader thread plus a queue works everywhere.
    """

    def __init__(self, proc):
        self.lines = queue.Queue()
        self.thread = threading.Thread(target=self._read, args=(proc,), daemon=True)
        self.thread.start()

    def _read(self, proc):
        try:
            for line in proc.stdout:
                self.lines.put(line.rstrip("\n"))
        except ValueError:
            pass

    def readline(self, deadline):
        """One line before deadline, or None on timeout / EOF."""
        try:
            return self.lines.get(timeout=max(0.0, deadline - time.time()))
        except queue.Empty:
            return None


def wait_for(lines, needle, timeout=10):
    """True when a line containing needle arrives before timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = lines.readline(deadline)
        if line is None:
            return False
        print(f"  polling example: {line}")
        if needle in line:
            return True
    return False


def chmod_check():
    """A metadata-only change reports Chmod, via the polling example."""
    if sys.platform == "win32":
        print("$ chmod check: skipped on Windows (backend does not "
              "watch file attributes, so a chmod reports no event)")
        return True
    print("$ chmod check (polling example, shell chmod)")
    CHMOD_DIR.mkdir(exist_ok=True)
    target = CHMOD_DIR / "mode.txt"
    proc = subprocess.Popen(
        ["goose", "run", "examples/watch_directory_polling.goose", "--",
         str(CHMOD_DIR)],
        cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1)
    lines = ProcLines(proc)
    try:
        if not wait_for(lines, "polling", timeout=30):
            print("FAIL: polling example did not start")
            return False
        target.write_text("x")
        if not wait_for(lines, " - create", timeout=10):
            print("FAIL: create did not arrive before the chmod")
            return False
        os.chmod(target, 0o600)
        if not wait_for(lines, " - chmod", timeout=10):
            print("FAIL: chmod did not arrive")
            return False
        print("ok")
        return True
    finally:
        proc.kill()
        proc.wait()


def main():
    clean()
    results = []
    try:
        # sys.executable, not "python3": the interpreter running this
        # script is guaranteed to exist on every runner, while
        # "python3" is not guaranteed on Windows runners.
        results.append(("api_check", run([sys.executable, "test/api_check.py"])))
        for target in ("watcher.goose", "examples/watch_directory_blocking.goose",
                       "examples/watch_directory_polling.goose",
                       "test/watcher_tests.goose"):
            results.append((f"goose check {target}", run(["goose", "check", target])))
        behaviour_cmd = ["goose", "run", "test/watcher_tests.goose"]
        if sys.platform == "win32":
            # Skip the overflow section: see the header of
            # test/watcher_tests.goose for why it is not valid there.
            # The "--" separates Goose's own arguments from the
            # program's; without it Goose reads the argument as a
            # second input file ("multiple input files given").
            behaviour_cmd.extend(["--", "skip-overflow"])
        results.append(("behaviour tests",
                        run(behaviour_cmd, timeout=300)))
        results.append(("chmod check", chmod_check()))
    finally:
        clean()
    print()
    for name, ok in results:
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
