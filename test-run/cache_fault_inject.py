#!/usr/bin/env python3
"""Fault injection for the quarantine lifecycle, without test hooks in the binary.

The plan requires injecting filesystem failures while keeping the production build free
of externally triggerable fault switches. On Windows a file that is open for reading
cannot be deleted or replaced - measured, see `hold_delete_probe.cpp` - so a separate
process holding a handle is a real failure injector that needs no cooperation from the
server.

That covers the paths that matter:

    fs::remove  failing   -> a cache entry cannot be deleted  (F01, F02)
    rename      failing   -> the quarantine record cannot be replaced (F03)

Usage:
    python test-run/cache_fault_inject.py hold <file> [seconds]
    python test-run/cache_fault_inject.py drop <cache-dir> <name>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cache_functional_matrix as M


def hold(path, seconds):
    """Open `path` for reading and keep it open, so the server cannot delete it.

    The handle is opened in this process and simply held. Nothing else is needed: the
    sharing mode a plain `open()` uses is enough to make a delete fail, which is the
    point - the injection is a property of the filesystem, not of the server.
    """
    if not os.path.exists(path):
        sys.exit(f'no such file to hold: {path}')

    size = os.path.getsize(path)
    print(f'holding {path} ({size} bytes) for {seconds}s', flush=True)

    f = open(path, 'rb')

    deadline = time.time() + seconds
    try:
        while time.time() < deadline:
            # read a little so the handle is clearly in use, then idle
            f.seek(0)
            f.read(4096)
            time.sleep(1.0)
    finally:
        f.close()

    print('released', flush=True)


def show_state(cache_dir):
    """Report what the cache directory and the server say about quarantine."""
    print(f'--- {cache_dir}')
    if os.path.isdir(cache_dir):
        for name in sorted(os.listdir(cache_dir)):
            p = os.path.join(cache_dir, name)
            print(f'  {name:<32} {os.path.getsize(p):>12} bytes')
    else:
        print('  (missing)')


COMMANDS = {'hold': hold, 'show': show_state}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]

    if cmd == 'hold':
        if len(sys.argv) < 3:
            sys.exit('usage: hold <file> [seconds]')
        hold(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 60.0)
    elif cmd == 'show':
        show_state(sys.argv[2] if len(sys.argv) > 2 else 'test-run/cache-fault')


if __name__ == '__main__':
    main()
