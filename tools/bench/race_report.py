#!/usr/bin/env python3
"""Summarises a Helgrind or DRD log written by race_check.sh.

usage: race_report.py <log>

A report is the engine's when one of the two conflicting accesses happens in
engine code (engine_i486.so), or on a thread the engine created (network and
snapshot encoding threads): code those threads call into, such as Metamod's
callbacks run by game DLL conditional encoders, is the engine's concern. Those
reports are listed. The others happen inside other libraries, in practice the
closed-source Steam client racing with its own threads, and are only counted.
Exit code 1 when a report is the engine's.
"""

import collections
import re
import sys

ENGINE = "engine_i486.so"
# Frames of the creation stack of the threads the engine starts
ENGINE_THREAD_STARTS = ("CWorkerPool::Start", "CNetReceiveThread")


def engine_threads(log):
    """Numbers of the threads created by the engine, from the thread announcements
    (Helgrind announces the threads of the reports, with their creation stack)."""
    threads = set()
    parts = re.split(r"Thread #(\d+) was created", log)
    for number, stack in zip(parts[1::2], parts[2::2]):
        stack = re.split(r"\n==\d+== *\n", stack, 1)[0]
        if any(start in stack for start in ENGINE_THREAD_STARTS):
            threads.add(number)
    return threads


def access_threads(block):
    """Thread numbers of the two conflicting accesses of a report."""
    return re.findall(r"(?:Possible data race|This conflicts with).*? by thread #(\d+)", block)


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

    threads = engine_threads(log)
    engine = collections.Counter()
    samples = {}
    others = 0
    for block in blocks:
        if "Possible data race" not in block and "Conflicting" not in block and "lock order" not in block:
            continue

        tops = access_top_frames(block)
        on_engine_thread = any(t in threads for t in access_threads(block))
        if not any(ENGINE in top for top in tops) and not on_engine_thread:
            others += 1
            continue

        symbols = [re.search(r": (.*?)(?: \(in |$)", top).group(1) for top in tops]
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
