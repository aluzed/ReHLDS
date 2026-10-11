#!/usr/bin/env python3
"""GoldSrc protocol load client: connects N fake players to a LAN server.

Each client performs the real handshake (challenge, connect, netchan with
reliable acks and fragment reassembly), signs on (new, sendres, spawn,
sendents), joins a team, then sends clc_move at a fixed command rate with
moving players and clc_delta acks, like a real client.

Server packets are only decoded during sign-on. In game, the 8-byte netchan
header is enough to acknowledge, which keeps the client cheap enough to drive
32 players from one Python process.

The server must run with sv_lan 1 (Steam tickets are not valid), mp_consistency 0
and sv_rehlds_maxclients_from_single_ip >= number of clients.

usage: loadclient.py --server 127.0.0.1:27015 --clients 32 --duration 60 --output clients.json
"""

import argparse
import bz2
import json
import math
import os
import random
import selectors
import socket
import struct
import sys
import time

PROTOCOL_VERSION = 48
OOB_HEADER = b"\xff\xff\xff\xff"

CLC_NOP = 1
CLC_MOVE = 2
CLC_STRINGCMD = 3
CLC_DELTA = 4

SVC_PRINT = 8
SVC_SERVERINFO = 11

IN_JUMP = 1 << 1

MUNGE_TABLE_1 = bytes([0x7A, 0x64, 0x05, 0xF1, 0x1B, 0x9B, 0xA0, 0xB5, 0xCA, 0xED, 0x61, 0x0D, 0x4A, 0xDF, 0x8E, 0xC7])
MUNGE_TABLE_2 = bytes([0x05, 0x61, 0x7A, 0xED, 0x1B, 0xCA, 0x0D, 0x9B, 0x4A, 0xF1, 0x64, 0xC7, 0xB5, 0x8E, 0xDF, 0xA0])
MUNGE_TABLE_3 = bytes([0x20, 0x07, 0x13, 0x61, 0x03, 0x45, 0x17, 0x72, 0x0A, 0x2D, 0x48, 0x0C, 0x4A, 0x12, 0xA9, 0xB5])


# --- COM_Munge / COM_UnMunge (engine/common.cpp) -----------------------------

def _munge_key(table, i):
    # Per-byte XOR mask of word i, as a little-endian 32-bit value
    key = 0
    for j in range(4):
        key |= ((0xA5 | (j << j) | j | table[(i + j) & 0x0F]) & 0xFF) << (8 * j)
    return key


def _bswap32(v):
    return int.from_bytes(v.to_bytes(4, "little"), "big")


def munge(data, seq, table):
    """In-place COM_Munge* on a bytearray (whole 32-bit words only)."""
    seq &= 0xFFFFFFFF
    not_seq = ~seq & 0xFFFFFFFF
    for i in range(len(data) // 4):
        c = struct.unpack_from("<I", data, i * 4)[0]
        c = _bswap32(c ^ not_seq) ^ _munge_key(table, i)
        struct.pack_into("<I", data, i * 4, c ^ seq)


def unmunge(data, seq, table):
    """In-place COM_UnMunge* on a bytearray (whole 32-bit words only)."""
    seq &= 0xFFFFFFFF
    not_seq = ~seq & 0xFFFFFFFF
    for i in range(len(data) // 4):
        c = struct.unpack_from("<I", data, i * 4)[0]
        c = _bswap32((c ^ seq) ^ _munge_key(table, i))
        struct.pack_into("<I", data, i * 4, c ^ not_seq)


# --- COM_BlockSequenceCRCByte (engine/crc.cpp) -------------------------------

def _crc_table():
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        table.append(c)
    return table


CRC_TABLE = _crc_table()
CRC_TABLE_BYTES = struct.pack("<256I", *CRC_TABLE)


def block_sequence_crc_byte(data, sequence):
    salt = CRC_TABLE_BYTES[sequence % 0x3FC: sequence % 0x3FC + 4]
    chunk = bytes(data[:60]) + salt
    crc = 0xFFFFFFFF
    for b in chunk:
        crc = CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return ~crc & 0xFF


# --- Bit writer (MSG_WriteBits, little-endian bit order) ---------------------

class BitWriter:
    def __init__(self):
        self.acc = 0
        self.nbits = 0

    def bits(self, value, numbits):
        limit = (1 << numbits) - 1
        if value > limit:
            value = limit
        self.acc |= (value & limit) << self.nbits
        self.nbits += numbits

    def sbits(self, value, numbits):
        maxnum = (1 << (numbits - 1)) - 1
        value = max(-maxnum, min(maxnum, value))
        self.bits(1 if value < 0 else 0, 1)
        self.bits(abs(value), numbits - 1)

    def angle(self, degrees, numbits):
        shift = 1 << numbits
        self.bits(int(shift * math.fmod(degrees, 360.0)) // 360 & (shift - 1), numbits)

    def to_bytes(self):
        return self.acc.to_bytes((self.nbits + 7) // 8, "little")


# usercmd_t fields, in delta.lst order (the bit index of each field in the send mask)
USERCMD_FIELDS = {
    "lerp_msec": 0,
    "msec": 1,
    "yaw": 2,          # viewangles[1]
    "pitch": 3,        # viewangles[0]
    "buttons": 4,
    "forwardmove": 5,
    "sidemove": 7,
}


def encode_usercmd(cmd):
    """Delta-encodes a usercmd against an all-zero command (DELTA_WriteDelta)."""
    marked = [name for name, value in cmd.items() if value]
    mask = 0
    for name in marked:
        mask |= 1 << USERCMD_FIELDS[name]

    w = BitWriter()
    bytecount = (mask.bit_length() + 7) // 8
    w.bits(bytecount, 3)
    for i in range(bytecount):
        w.bits((mask >> (8 * i)) & 0xFF, 8)

    for name in sorted(marked, key=lambda n: USERCMD_FIELDS[n]):
        value = cmd[name]
        if name == "lerp_msec":
            w.bits(value, 9)
        elif name == "msec":
            w.bits(value, 8)
        elif name in ("yaw", "pitch"):
            w.angle(value, 16)
        elif name == "buttons":
            w.bits(value, 16)
        else:  # forwardmove / sidemove: DT_SIGNED | DT_FLOAT, 12 bits
            w.sbits(int(value), 12)
    return w.to_bytes()


# --- Netchan (engine/net_chan.cpp) ---------------------------------------------

class Netchan:
    def __init__(self):
        self.outgoing_sequence = 1
        self.incoming_sequence = 0
        self.incoming_acknowledged = 0
        self.incoming_reliable_acknowledged = 0
        self.incoming_reliable_sequence = 0
        self.reliable_sequence = 0
        self.last_reliable_sequence = 0
        self.reliable_buf = b""
        self.pending_reliable = []
        self.fragments = {}  # fragment id -> data, normal stream only

    def queue_reliable(self, data):
        self.pending_reliable.append(data)

    def transmit(self, unreliable=b""):
        send_reliable = False
        if (self.incoming_acknowledged > self.last_reliable_sequence
                and self.incoming_reliable_acknowledged != self.reliable_sequence):
            send_reliable = True  # resend the unacknowledged reliable message

        if not self.reliable_buf and self.pending_reliable:
            self.reliable_buf = b"".join(self.pending_reliable)
            self.pending_reliable = []
            self.reliable_sequence ^= 1
            send_reliable = True

        w1 = self.outgoing_sequence | (int(send_reliable) << 31)
        w2 = self.incoming_sequence | (self.incoming_reliable_sequence << 31)
        sequence = self.outgoing_sequence
        self.outgoing_sequence += 1

        payload = bytearray(self.reliable_buf if send_reliable else b"")
        if send_reliable:
            self.last_reliable_sequence = sequence
        payload += unreliable
        while len(payload) + 8 < 16:
            payload.append(CLC_NOP)

        munge(payload, sequence & 0xFF, MUNGE_TABLE_2)
        return struct.pack("<II", w1, w2) + bytes(payload), sequence

    def process(self, packet, decode):
        """Updates the channel from a server packet.

        Returns the payload with fragments removed when decode is set, and a
        completed normal-stream fragment buffer if one finished.
        """
        if len(packet) < 8:
            return None, None
        sequence, sequence_ack = struct.unpack_from("<II", packet)
        reliable_message = sequence >> 31
        reliable_ack = sequence_ack >> 31
        has_fragments = bool(sequence & (1 << 30))
        sequence &= 0x3FFFFFFF
        sequence_ack &= 0x3FFFFFFF

        if sequence <= self.incoming_sequence:
            return None, None  # duplicate or out of order

        if reliable_ack == self.reliable_sequence and sequence_ack >= self.last_reliable_sequence:
            self.reliable_buf = b""  # acknowledged

        self.incoming_sequence = sequence
        self.incoming_acknowledged = sequence_ack
        self.incoming_reliable_acknowledged = reliable_ack
        if reliable_message:
            self.incoming_reliable_sequence ^= 1

        if not decode:
            return None, None

        data = bytearray(packet[8:])
        unmunge(data, sequence & 0xFF, MUNGE_TABLE_2)

        completed = None
        if has_fragments:
            pos = 0
            frags = []
            for stream in range(2):
                if data[pos]:
                    fragid, offset, length = struct.unpack_from("<IHH", data, pos + 1)
                    frags.append((stream, fragid, offset, length))
                    pos += 9
                else:
                    pos += 1
            body = data[pos:]
            removed = 0
            for stream, fragid, offset, length in frags:
                start = offset - removed
                chunk = bytes(body[start:start + length])
                del body[start:start + length]
                removed += length
                if stream == 0 and fragid:
                    completed = self._add_fragment(fragid, chunk) or completed
            data = body
        return data, completed

    def _add_fragment(self, fragid, chunk):
        index, count = fragid >> 16, fragid & 0xFFFF
        self.fragments[index] = chunk
        if all(i in self.fragments for i in range(1, count + 1)):
            buf = b"".join(self.fragments[i] for i in range(1, count + 1))
            self.fragments = {}
            if buf[:4] == b"BZ2\x00":
                buf = bz2.decompress(buf[4:])
            return buf
        return None


# --- Client --------------------------------------------------------------------

def read_string(buf, pos):
    end = buf.index(0, pos)
    return buf[pos:end].decode("latin-1"), end + 1


def parse_serverinfo(buf):
    """Finds svc_serverinfo at the start of the 'new' response."""
    pos = 0
    while pos < len(buf):
        cmd = buf[pos]
        pos += 1
        if cmd == SVC_PRINT:
            _, pos = read_string(buf, pos)
        elif cmd == SVC_SERVERINFO:
            protocol, spawncount, munged_crc = struct.unpack_from("<iiI", buf, pos)
            pos += 12 + 16  # + client dll MD5
            maxclients, playernum = buf[pos], buf[pos + 1]
            return spawncount, munged_crc, playernum
        else:
            break
    raise ValueError("svc_serverinfo not found in sign-on data")


class Client:
    def __init__(self, index, server, cmdrate, stats):
        self.index = index
        self.server = server
        self.cmdrate = cmdrate
        self.stats = stats
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.sock.bind(("0.0.0.0", 0))
        self.state = "idle"
        self.netchan = None
        self.next_send = 0.0
        self.state_since = 0.0
        self.yaw = random.uniform(0, 360)
        self.last_receive = None
        self.intervals = []
        self.ingame_commands = []

    def log(self, message):
        print(f"[client {self.index:02d}] {message}", file=sys.stderr)

    def send_oob(self, text, extra=b""):
        self.sock.sendto(OOB_HEADER + text.encode("latin-1") + extra, self.server)

    def start(self, now):
        self.state = "challenge"
        self.state_since = now
        self.send_oob("getchallenge steam\n")
        self.next_send = now + 1.0

    def stringcmd(self, text):
        self.netchan.queue_reliable(bytes([CLC_STRINGCMD]) + text.encode("latin-1") + b"\x00")

    def set_state(self, state, now):
        self.state = state
        self.state_since = now

    def on_packet(self, packet, now):
        if packet[:4] == OOB_HEADER:
            self.on_oob(packet[4:], now)
            return

        if not self.netchan:
            return

        decode = self.state != "ingame"
        _, completed = self.netchan.process(packet, decode)

        if self.state == "ingame":
            if self.last_receive is not None:
                self.intervals.append(now - self.last_receive)
            self.last_receive = now
            return

        if completed is None:
            return

        if self.state == "new":
            spawncount, munged_crc, playernum = parse_serverinfo(completed)
            crc = bytearray(struct.pack("<I", munged_crc))
            unmunge(crc, (-1 - playernum) & 0xFF, MUNGE_TABLE_3)
            munge(crc, (-1 - spawncount) & 0xFF, MUNGE_TABLE_2)
            self.spawn_args = f"{spawncount} {struct.unpack('<i', bytes(crc))[0]}"
            self.stringcmd("sendres")
            self.set_state("sendres", now)
        elif self.state == "sendres":
            self.stringcmd(f"spawn {self.spawn_args}")
            self.set_state("spawn", now)
        elif self.state == "spawn":
            self.stringcmd("sendents")
            self.stringcmd("jointeam 5")
            self.stringcmd("joinclass 5")
            self.set_state("ingame", now)
            self.stats["ingame"] += 1
            self.log("in game")

    def on_oob(self, payload, now):
        text = payload.decode("latin-1", "replace")
        if text.startswith("A") and self.state == "challenge":
            challenge = int(text.split()[1])
            protinfo = "\\prot\\3\\unique\\-1\\raw\\steam\\cdkey\\" + os.urandom(16).hex()
            userinfo = (f"\\name\\load{self.index:02d}\\rate\\100000\\cl_updaterate\\100"
                        f"\\cl_lw\\1\\cl_lc\\1\\cl_dlmax\\1024\\model\\gsg9\\_vgui_menus\\0")
            ticket = os.urandom(32)
            self.send_oob(f'connect {PROTOCOL_VERSION} {challenge} "{protinfo}" "{userinfo}"\n', ticket)
            self.set_state("connect", now)
        elif text.startswith("B") and self.state == "connect":
            self.netchan = Netchan()
            self.stringcmd("new")
            self.set_state("new", now)
            self.next_send = now
        elif text.startswith("9") or "reject" in text.lower():
            self.log(f"rejected: {text.strip()[:120]}")
            self.set_state("failed", now)
            self.stats["failed"] += 1

    def build_move(self, sequence, now):
        self.yaw = (self.yaw + random.uniform(-4, 4)) % 360
        cmd = {
            "lerp_msec": 100,
            "msec": int(1000 / self.cmdrate),
            "yaw": self.yaw,
            "forwardmove": 250,
            "sidemove": random.choice((0, 0, 0, 250, -250)),
            "buttons": IN_JUMP if random.random() < 0.01 else 0,
        }
        region = bytearray([0, 0, 1]) + encode_usercmd(cmd)  # loss, backup, numcmds
        checksum = block_sequence_crc_byte(region, sequence)
        munge(region, sequence, MUNGE_TABLE_1)
        return bytes([CLC_MOVE, len(region), checksum]) + region

    def tick(self, now):
        """Sends the next packet if due. Returns the time of the next send."""
        if self.state in ("idle", "failed"):
            return None

        if self.state != "ingame" and now - self.state_since > 15.0:
            self.log(f"timed out in state {self.state}")
            self.set_state("failed", now)
            self.stats["failed"] += 1
            return None

        if now < self.next_send:
            return self.next_send

        if self.state in ("challenge", "connect"):
            # Retry the out-of-band step
            if self.state == "challenge":
                self.send_oob("getchallenge steam\n")
            self.next_send = now + 1.0
            return self.next_send

        sequence = self.netchan.outgoing_sequence
        unreliable = b""
        if self.state == "ingame":
            unreliable = self.build_move(sequence, now)
            unreliable += bytes([CLC_DELTA, self.netchan.incoming_sequence & 0xFF])
        packet, _ = self.netchan.transmit(unreliable)
        self.sock.sendto(packet, self.server)

        interval = 1.0 / self.cmdrate if self.state == "ingame" else 0.02
        self.next_send = max(self.next_send + interval, now - interval)
        return self.next_send


def percentile(values, p):
    if not values:
        return None
    s = sorted(values)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))]


def summarize(clients, duration):
    intervals = [v * 1000.0 for c in clients for v in c.intervals]
    per_client_rate = [len(c.intervals) / duration for c in clients if c.intervals]
    summary = {
        "clients": len(clients),
        "ingame": sum(1 for c in clients if c.state == "ingame"),
        "snapshots": len(intervals),
        "snapshot_rate_per_client": sum(per_client_rate) / len(per_client_rate) if per_client_rate else 0.0,
    }
    if intervals:
        avg = sum(intervals) / len(intervals)
        summary.update({
            "interval_ms_avg": avg,
            "interval_ms_p50": percentile(intervals, 0.50),
            "interval_ms_p95": percentile(intervals, 0.95),
            "interval_ms_p99": percentile(intervals, 0.99),
            "interval_ms_max": max(intervals),
            "interval_ms_stddev": math.sqrt(sum((v - avg) ** 2 for v in intervals) / len(intervals)),
        })
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", default="127.0.0.1:27015")
    parser.add_argument("--clients", type=int, default=32)
    parser.add_argument("--cmdrate", type=int, default=100)
    parser.add_argument("--stagger", type=float, default=0.25, help="seconds between client connections")
    parser.add_argument("--warmup", type=float, default=10.0, help="seconds in game before measuring")
    parser.add_argument("--measure-at", type=float,
                        help="wall clock time (epoch seconds) at which measuring starts, instead of --warmup")
    parser.add_argument("--duration", type=float, default=30.0, help="measured seconds")
    parser.add_argument("--output", help="write the JSON summary to this file")
    args = parser.parse_args()

    host, port = args.server.rsplit(":", 1)
    server = (socket.gethostbyname(host), int(port))
    stats = {"ingame": 0, "failed": 0}
    clients = [Client(i + 1, server, args.cmdrate, stats) for i in range(args.clients)]

    sel = selectors.DefaultSelector()
    for c in clients:
        sel.register(c.sock, selectors.EVENT_READ, c)

    start = time.monotonic()
    connect_times = [start + i * args.stagger for i in range(len(clients))]
    measure_start = None
    measure_end = None

    while True:
        now = time.monotonic()

        for i, c in enumerate(clients):
            if c.state == "idle" and now >= connect_times[i]:
                c.start(now)

        settled = stats["ingame"] + stats["failed"] == len(clients)
        if measure_start is None and args.measure_at is not None:
            measure_start = now + max(0.0, args.measure_at - time.time())
            measure_end = measure_start + args.duration
        if measure_start is None and (settled or now - start > 60.0):
            measure_start = now + args.warmup
            measure_end = measure_start + args.duration
            print(f"[loadclient] {stats['ingame']}/{len(clients)} clients in game, measuring "
                  f"{args.duration:.0f} s after {args.warmup:.0f} s warm-up", file=sys.stderr)
            if stats["ingame"] == 0 and args.measure_at is None:
                break
        if measure_start is not None and now >= measure_start and not getattr(main, "reset", False):
            for c in clients:
                c.intervals = []
            main.reset = True
        if measure_end is not None and now >= measure_end:
            break

        next_times = [t for t in (c.tick(now) for c in clients) if t is not None]
        next_times += [t for i, t in enumerate(connect_times) if clients[i].state == "idle"]
        timeout = max(0.0, min(next_times) - time.monotonic()) if next_times else 0.05

        for key, _ in sel.select(min(timeout, 0.05)):
            c = key.data
            while True:
                try:
                    packet, _ = c.sock.recvfrom(65535)
                except BlockingIOError:
                    break
                c.on_packet(packet, time.monotonic())

    summary = summarize(clients, args.duration)
    text = json.dumps(summary, indent=2)
    print(text)
    if args.output:
        with open(args.output, "w") as f:
            f.write(text + "\n")
    return 0 if summary["ingame"] else 1


if __name__ == "__main__":
    sys.exit(main())
