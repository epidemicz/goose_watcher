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
"""

import os
import select
import shutil
import subprocess
import sys
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


def read_line_with_deadline(proc, deadline):
    """One line from proc.stdout before deadline, or None on timeout."""
    fd = proc.stdout.fileno()
    buf = ""
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], max(0.0, deadline - time.time()))
        if not ready:
            return None
        chunk = os.read(fd, 4096).decode(errors="replace")
        if not chunk:
            return buf or None
        buf += chunk
        if "\n" in buf:
            line, _ = buf.split("\n", 1)
            return line
    return None


def wait_for(proc, needle, timeout=10):
    """True when a line containing needle arrives before timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = read_line_with_deadline(proc, deadline)
        if line is None:
            return False
        print(f"  polling example: {line}")
        if needle in line:
            return True
    return False


def chmod_check():
    """A metadata-only change reports Chmod, via the polling example."""
    print("$ chmod check (polling example, shell chmod)")
    CHMOD_DIR.mkdir(exist_ok=True)
    target = CHMOD_DIR / "mode.txt"
    proc = subprocess.Popen(
        ["goose", "run", "examples/watch_directory_polling.goose", "--",
         str(CHMOD_DIR)],
        cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1)
    try:
        if not wait_for(proc, "polling", timeout=30):
            print("FAIL: polling example did not start")
            return False
        target.write_text("x")
        if not wait_for(proc, " - create", timeout=10):
            print("FAIL: create did not arrive before the chmod")
            return False
        os.chmod(target, 0o600)
        if not wait_for(proc, " - chmod", timeout=10):
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
        results.append(("api_check", run(["python3", "test/api_check.py"])))
        for target in ("watcher.goose", "examples/watch_directory_blocking.goose",
                       "examples/watch_directory_polling.goose",
                       "test/watcher_tests.goose"):
            results.append((f"goose check {target}", run(["goose", "check", target])))
        results.append(("behaviour tests",
                        run(["goose", "run", "test/watcher_tests.goose"], timeout=300)))
        results.append(("chmod check", chmod_check()))
    finally:
        clean()
    print()
    for name, ok in results:
        print(f"{'ok  ' if ok else 'FAIL'} {name}")
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
