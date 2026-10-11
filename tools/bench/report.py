#!/usr/bin/env python3
"""Builds a Markdown report from a benchmark result directory.

usage: report.py <result dir> [--baseline <result dir>]

The result directory contains profile.csv (written by sv_profile_csv), cpu.txt
and meta.txt (written by run.sh). With --baseline, every statistic is shown with
its relative change against the baseline run.
"""

import argparse
import csv
import math
import os
import sys

# Columns shown in the time table, with their nesting depth (same tree as sv_profile)
PHASES = [
    ("tick_interval_us", 0),
    ("frame_us", 0),
    ("cbuf_us", 1),
    ("sv_frame_us", 1),
    ("read_packets_us", 2),
    ("connectionless_us", 3),
    ("netchan_process_us", 3),
    ("execute_client_msg_us", 3),
    ("physics_us", 2),
    ("check_timeouts_us", 2),
    ("send_messages_us", 2),
    ("datagram_us", 3),
    ("gather_us", 4),
    ("encode_us", 4),
    ("transmit_us", 3),
    ("steam_us", 2),
    ("log_write_us", 1),
]

COUNTERS = [
    "filter_rejects",
    "packets_in",
    "bytes_in",
    "packets_out",
    "bytes_out",
    "clients_sent",
    "entities_sent",
]


def read_key_values(path):
    values = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                if "=" in line:
                    key, value = line.rstrip("\n").split("=", 1)
                    values[key] = value
    return values


def read_columns(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit(f"{path}: no frames")
    return {name: [float(row[name]) for row in rows] for name in rows[0]}


def percentile(sorted_values, p):
    index = int(p * (len(sorted_values) - 1) + 0.5)
    return sorted_values[index]


def stats(values):
    n = len(values)
    avg = sum(values) / n
    stddev = math.sqrt(sum((v - avg) ** 2 for v in values) / n)
    s = sorted(values)
    return {
        "avg": avg,
        "p50": percentile(s, 0.50),
        "p95": percentile(s, 0.95),
        "p99": percentile(s, 0.99),
        "max": s[-1],
        "stddev": stddev,
    }


class Run:
    def __init__(self, path):
        self.path = path
        self.meta = read_key_values(os.path.join(path, "meta.txt"))
        self.cpu = read_key_values(os.path.join(path, "cpu.txt"))
        self.columns = read_columns(os.path.join(path, "profile.csv"))
        self.stats = {name: stats(values) for name, values in self.columns.items()}
        self.frames = len(self.columns["tick_interval_us"])

    @property
    def seconds(self):
        return sum(self.columns["tick_interval_us"]) / 1e6

    @property
    def fps(self):
        return self.frames / self.seconds if self.seconds else 0.0

    @property
    def cpu_percent(self):
        try:
            ticks = int(self.cpu["cpu_ticks"])
            clk_tck = int(self.cpu["clk_tck"])
            wall = float(self.cpu["wall_end"]) - float(self.cpu["wall_start"])
        except (KeyError, ValueError):
            return None
        return 100.0 * ticks / clk_tck / wall if wall > 0 else None


def fmt_change(value, base):
    if base is None:
        return ""
    if base == 0:
        return "" if value == 0 else " (new)"
    return f" ({100.0 * (value - base) / base:+.0f}%)"


def cell(value, base, decimals=1):
    return f"{value:.{decimals}f}{fmt_change(value, base)}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("result")
    parser.add_argument("--baseline")
    args = parser.parse_args()

    run = Run(args.result)
    base = Run(args.baseline) if args.baseline else None

    def base_value(column, key):
        return base.stats[column][key] if base and column in base.stats else None

    print(f"# Benchmark: {run.meta.get('scenario', '?')} / engine {run.meta.get('engine', '?')}")
    print()
    if base:
        print(f"Baseline: {base.meta.get('scenario', '?')} / engine {base.meta.get('engine', '?')} "
              f"(`{os.path.basename(base.path)}`)")
        print()
    print(f"* Commit: `{run.meta.get('engine_commit', '?')}`")
    print(f"* CPU: {run.meta.get('cpu', '?')}, governor {run.meta.get('governor', '?')}, "
          f"kernel {run.meta.get('kernel', '?')}")
    if run.meta.get("server_args"):
        print(f"* Server args: `{run.meta['server_args']}`")
    if run.meta.get("docker_args"):
        print(f"* Docker args: `{run.meta['docker_args']}`")
    print()

    cpu = run.cpu_percent
    base_cpu = base.cpu_percent if base else None
    busy = 100.0 * run.stats["frame_us"]["avg"] / run.stats["tick_interval_us"]["avg"]
    base_busy = (100.0 * base.stats["frame_us"]["avg"] / base.stats["tick_interval_us"]["avg"]) if base else None

    print("| Summary | Value |")
    print("|---|---|")
    print(f"| Frames | {run.frames} over {run.seconds:.1f} s |")
    print(f"| Server FPS | {cell(run.fps, base.fps if base else None)} |")
    print(f"| Tick jitter (stddev) | {cell(run.stats['tick_interval_us']['stddev'], base_value('tick_interval_us', 'stddev'))} us |")
    print(f"| Busy (frame / tick) | {cell(busy, base_busy)} % |")
    if cpu is not None:
        print(f"| Process CPU | {cell(cpu, base_cpu)} % of one core |")
    print()

    print("| Time (us) | avg | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|")
    for column, depth in PHASES:
        if column not in run.stats:
            continue
        s = run.stats[column]
        label = "&nbsp;&nbsp;" * depth + column[:-3]
        values = " | ".join(cell(s[key], base_value(column, key)) for key in ("avg", "p50", "p95", "p99", "max"))
        print(f"| {label} | {values} |")
    print()

    print("| Per frame | avg | p99 | max |")
    print("|---|---|---|---|")
    for column in COUNTERS:
        if column not in run.stats:
            continue
        s = run.stats[column]
        print(f"| {column} | {cell(s['avg'], base_value(column, 'avg'), 2)} | {s['p99']:.0f} | {s['max']:.0f} |")


if __name__ == "__main__":
    main()
