#!/usr/bin/env python3
"""Runs a command with a ceiling on its memory and its time.

For experiments that can run away: a compile that grows without end will take
the machine's memory and then its swap long before it fails on its own. This
watches the command and everything it starts, once a second, and kills the lot
the moment their resident memory together passes --mem or the clock passes
--time. Its exit status says which: 0 or the command's own, 124 for time, 125
for memory.

usage: tools/guarded.py --mem 16 --time 600 -- COMMAND [ARGS...]
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time


def tree_rss_bytes(root: int) -> int:
    """Resident memory of `root` and all its descendants, from ps."""
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,rss="], capture_output=True,
                         text=True).stdout
    children: dict[int, list[int]] = {}
    rss: dict[int, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        pid, ppid, kb = map(int, parts)
        children.setdefault(ppid, []).append(pid)
        rss[pid] = kb
    total, stack = 0, [root]
    while stack:
        pid = stack.pop()
        total += rss.get(pid, 0)
        stack.extend(children.get(pid, []))
    return total * 1024


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mem", type=float, required=True, help="GB of resident memory")
    parser.add_argument("--time", type=float, required=True, help="seconds")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command")

    # Its own process group, so one signal ends it and everything it started.
    proc = subprocess.Popen(command, start_new_session=True)
    started = time.monotonic()
    peak = 0
    reason = None
    while proc.poll() is None:
        used = tree_rss_bytes(proc.pid)
        peak = max(peak, used)
        if used > args.mem * 1e9:
            reason = f"memory {used / 1e9:.1f} GB passed {args.mem:g} GB"
        elif time.monotonic() - started > args.time:
            reason = f"time passed {args.time:g} s"
        if reason:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
            break
        time.sleep(1.0)

    elapsed = time.monotonic() - started
    if reason:
        print(f"guarded: killed after {elapsed:.0f} s, {reason}", file=sys.stderr)
        return 125 if reason.startswith("memory") else 124
    print(f"guarded: finished in {elapsed:.0f} s, peak {peak / 1e9:.1f} GB", file=sys.stderr)
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
