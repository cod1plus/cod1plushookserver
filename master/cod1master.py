#!/usr/bin/env python3
"""
cod1master.py - minimal master server for cod1reloaded (CoD1).

Speaks the Q3/dpmaster connectionless (OOB) protocol that cod_lnxded and the
CoDMP.exe browser already use:
  - records game servers that send  "heartbeat COD-1"   (drops on "flatline")
  - answers client "getservers ..." with a "getserversResponse" packet

No external dependencies. Run:  python3 cod1master.py
Env:
  COD1MASTER_PORT       UDP port to listen on            (default 20510)
  COD1MASTER_PUBLIC_IP  advertise this IP when a server  (default unset)
                        heartbeats from a private/loopback address
                        (set this when the master runs on the SAME box as the
                         game server, e.g. COD1MASTER_PUBLIC_IP=87.106.7.52)
  COD1MASTER_TIMEOUT    drop servers unseen for N seconds (default 300)
"""

import os
import socket
import struct
import time

OOB = b"\xff\xff\xff\xff"
PORT = int(os.environ.get("COD1MASTER_PORT", "20510"))
PUBLIC_IP = os.environ.get("COD1MASTER_PUBLIC_IP", "").strip()
TIMEOUT = int(os.environ.get("COD1MASTER_TIMEOUT", "300"))

servers = {}  # (ip, port) -> last_seen_epoch


def log(*a):
    print(time.strftime("[%H:%M:%S]"), *a, flush=True)


def is_private(ip):
    if ip.startswith("127.") or ip.startswith("10.") or ip.startswith("192.168."):
        return True
    if ip.startswith("172."):
        try:
            return 16 <= int(ip.split(".")[1]) <= 31
        except (ValueError, IndexError):
            return False
    return False


def prune(now):
    for key in [k for k, t in servers.items() if now - t > TIMEOUT]:
        del servers[key]
        log("expired", "%s:%d" % key)


# EXACT wire format, specified by Cato (cod.pm author) 2026-08-24:
#   \xFF\xFF\xFF\xFF getserversResponse \n \x00 <\ + 4b ip + 2b port>... \EOF
# Two things used to be wrong and made cod.pm reject our list:
#   1. the "\n\0" separator after the header was MISSING (we went straight to the
#      first entry). The game client does not care - it scans for the next '\' -
#      but an aggregator that matches the header exactly sees a malformed packet.
#   2. the terminator was padded with three NUL bytes ("\EOF\0\0\0"), a
#      dpmaster-ism; the CoD convention ends at "\EOF" exactly.
# \EOF = end of the whole list. \EOT = end of THIS packet, more follow - sending
# \EOT on the last packet makes aggregators wait for a continuation that never
# comes (that bug shipped on the VPS for weeks).
MAX_PACKET = 1200          # keep each UDP datagram well under a typical MTU
ENTRY_LEN = 7              # 1 backslash + 4 bytes IP + 2 bytes port
RESP_HEADER = OOB + b"getserversResponse\n\0"


def build_responses():
    """Return the list of packets to send (1 in practice, more if many servers)."""
    per_packet = max(1, (MAX_PACKET - len(RESP_HEADER) - 4) // ENTRY_LEN)

    entries = [b"\\" + socket.inet_aton(ip) + struct.pack(">H", port)
               for ip, port in servers]

    chunks = [entries[i:i + per_packet] for i in range(0, len(entries), per_packet)] or [[]]
    packets = []
    for n, chunk in enumerate(chunks):
        last = (n == len(chunks) - 1)
        packets.append(RESP_HEADER + b"".join(chunk) +
                       (b"\\EOF" if last else b"\\EOT"))
    return packets


def handle(sock, data, addr, now):
    msg = data[4:]
    src_ip, src_port = addr

    if msg.startswith(b"heartbeat"):
        ip = PUBLIC_IP if (PUBLIC_IP and is_private(src_ip)) else src_ip
        key = (ip, src_port)
        if b"flatline" in msg:
            if key in servers:
                del servers[key]
                log("server gone (flatline)", "%s:%d" % key)
        else:
            fresh = key not in servers
            servers[key] = now
            if fresh:
                name = msg.split(b"\n", 1)[0].decode("latin-1", "replace")
                log("server up", "%s:%d" % key, "(%s)" % name)

    elif msg.startswith(b"getservers"):
        prune(now)
        # "getservers <protocol> [empty] [full]" - the requested protocol is
        # logged but never filtered on: the heartbeat carries no protocol, so
        # the only honest answer is "here is everything I know about".
        # truncated: the protocol is attacker-controlled free text, and an
        # untruncated log line is a trivial way to flood the journal
        args = msg.split(b"\n", 1)[0].split()[1:]
        proto = args[0][:16].decode("latin-1", "replace") if args else "-"
        packets = build_responses()
        for pkt in packets:
            sock.sendto(pkt, addr)
        log("getservers from", "%s:%d" % addr, "proto=%s" % proto,
            "-> %d server(s) in %d packet(s)" % (len(servers), len(packets)))


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", PORT))
    log("cod1master listening on UDP %d (public_ip=%s timeout=%ds)"
        % (PORT, PUBLIC_IP or "-", TIMEOUT))

    while True:
        try:
            data, addr = s.recvfrom(2048)
        except OSError as e:
            log("recv error", e)
            continue
        if not data.startswith(OOB):
            continue

        # Nothing below may ever kill the loop. cod.pm's parser gives us a ONE
        # SECOND timeout (Cato, 2026-08-24): a process that died on a single bad
        # packet - or on a sendto() hitting ENETUNREACH/EMSGSIZE because a client
        # vanished mid-answer - means the whole list drops off his site until a
        # human notices. One malformed datagram costs one log line, not the
        # service. (systemd Restart=always covers the rest; see cod1master.service)
        try:
            handle(s, data, addr, time.time())
        except Exception as e:                       # deliberate catch-all
            log("handler error from %s:%d:" % addr, repr(e))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
