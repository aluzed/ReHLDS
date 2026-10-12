#!/usr/bin/env python3
"""Compares the packets sent by two runs of the same session (MT-004).

usage: replay_compare.py <reference sends> <other sends>

Sends files are written by the engine with -netsends: every packet sent by the
server socket, with the frame it was sent in and its destination. Packets are
compared per destination, in order: with several threads, the order between
destinations may change, the order towards one destination must not.

Exit code 0 when both runs sent exactly the same bytes to every destination.
"""

import struct
import sys
from collections import OrderedDict

MAGIC = b"RHSENDS1"
HEADER = struct.Struct("<IIII")  # frame, ip, port, size


def read_sends(path):
    by_destination = OrderedDict()
    total = 0
    with open(path, "rb") as f:
        if f.read(8) != MAGIC:
            sys.exit(f"{path}: not a sends file")
        while True:
            header = f.read(HEADER.size)
            if len(header) < HEADER.size:
                break
            frame, ip, port, size = HEADER.unpack(header)
            data = f.read(size)
            if len(data) < size:
                break
            by_destination.setdefault((ip, port), []).append((frame, data))
            total += 1
    return by_destination, total


def address(destination):
    ip, port = destination
    octets = struct.pack("<I", ip)
    port = struct.unpack(">H", struct.pack("<H", port))[0]
    return f"{'.'.join(str(o) for o in octets)}:{port}"


def first_difference(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i
    return min(len(a), len(b))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)

    ref, ref_total = read_sends(sys.argv[1])
    other, other_total = read_sends(sys.argv[2])

    print(f"reference: {ref_total} packets to {len(ref)} destinations")
    print(f"other:     {other_total} packets to {len(other)} destinations")

    differences = 0
    for destination in OrderedDict.fromkeys(list(ref) + list(other)):
        a = ref.get(destination, [])
        b = other.get(destination, [])
        for i in range(max(len(a), len(b))):
            if i >= len(a) or i >= len(b):
                print(f"  {address(destination)}: {len(a)} packets vs {len(b)}")
                differences += 1
                break
            (frame_a, data_a), (frame_b, data_b) = a[i], b[i]
            if data_a != data_b:
                offset = first_difference(data_a, data_b)
                print(f"  {address(destination)}: packet {i} differs (frame {frame_a} vs {frame_b}, "
                      f"{len(data_a)} vs {len(data_b)} bytes, first difference at byte {offset})")
                differences += 1
                break
            if frame_a != frame_b:
                print(f"  {address(destination)}: packet {i} sent in frame {frame_b} instead of {frame_a}")
                differences += 1
                break

    if differences:
        print(f"DIFFERENT: {differences} of {len(ref)} destinations")
        return 1

    print("IDENTICAL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
