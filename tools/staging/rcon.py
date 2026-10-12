#!/usr/bin/env python3
"""Sends an RCON command to a GoldSrc server and prints the answer.

usage: rcon.py [--server 127.0.0.1:27015] --password-file <file> <command...>
"""

import argparse
import socket
import sys

HEADER = b"\xff\xff\xff\xff"


def request(sock, server, payload, timeout):
    sock.settimeout(timeout)
    sock.sendto(HEADER + payload, server)
    return sock.recvfrom(65535)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", default="127.0.0.1:27015")
    parser.add_argument("--password-file", required=True)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()

    host, port = args.server.rsplit(":", 1)
    server = (host, int(port))
    password = open(args.password_file).read().strip()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    reply = request(sock, server, b"challenge rcon\n", args.timeout)
    # "challenge rcon <number>\n"
    challenge = reply[4:].split()[2].decode()

    command = " ".join(args.command)
    sock.sendto(HEADER + f'rcon {challenge} "{password}" {command}\n'.encode(), server)

    # The answer may come in several packets; stop after a short silence
    sock.settimeout(0.5)
    output = b""
    try:
        while True:
            data = sock.recvfrom(65535)[0]
            if data.startswith(HEADER + b"l"):
                output += data[5:].rstrip(b"\x00")
    except socket.timeout:
        pass

    sys.stdout.write(output.decode(errors="replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
