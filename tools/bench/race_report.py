#!/usr/bin/env python3
"""Summarises a Helgrind or DRD log written by race_check.sh.

usage: race_report.py <log>

A report is the engine's when one of the two conflicting accesses happens in
engine code (engine_i486.so); those are listed. The others happen inside other
libraries, in practice the closed-source Steam client racing with its own
threads, and are only counted. Exit code 1 when a report is the engine's.
"""

import collections
import re
import sys

ENGINE = "engine_i486.so"


def access_top_frames(block):
    """Top frame of each access of a report: the racing access and the one it conflicts with."""
    tops = []
    expect_top = False
    for line in block.splitlines():
        if "Possible data race" in line or "This conflicts with" in line or "Conflicting" in line:
            expect_top = True
        elif expect_top and re.search(r"(?:at|by) 0x[0-9A-F]+:", line):
            tops.append(line)
            expect_top = False
    return tops


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)

    log = open(sys.argv[1], errors="replace").read()
    blocks = re.split(r"==\d+== -{10,}", log)

    engine = collections.Counter()
    samples = {}
    others = 0
    for block in blocks:
        if "Possible data race" not in block and "Conflicting" not in block and "lock order" not in block:
            continue

        tops = access_top_frames(block)
        if not any(ENGINE in top for top in tops):
            others += 1
            continue

        symbols = [re.search(r": (.*?) \(in ", top).group(1) for top in tops if ENGINE in top]
        key = " / ".join(symbols[:3])
        engine[key] += 1
        samples.setdefault(key, block)

    print(f"reports in engine code: {sum(engine.values())}")
    print(f"reports in other libraries: {others}")
    for key, count in engine.most_common():
        print(f"  {count} x {key}")
    for key, block in list(samples.items())[:3]:
        print(f"\n===== {key}")
        print("\n".join(l for l in block.splitlines() if l.strip("= ")))

    return 1 if engine else 0


if __name__ == "__main__":
    sys.exit(main())
