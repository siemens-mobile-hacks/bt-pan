#!/usr/bin/env python3
"""BT PAN speed test server for the phone elf (started by nap.sh).

UDP 5001 - "direct" test: the elf builds raw UDP itself, bypassing the phone's stack.
    "BTPAN DL <secs>"  flood 1472-byte datagrams to the sender for <secs> seconds
    "BTPAN STOP"       stop the flood
    anything else      upload data, counted and discarded
TCP 5003 - "stack" test through the phone's TCP/IP stack:
    "DL\\n"  send data until the phone closes (max 30 s)
    "UL\\n"  read and discard until the phone closes
"""
import socket
import sys
import threading
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.7.1"
UDP_PORT, TCP_PORT = 5001, 5003
CHUNK = b"BTPANDATA" + b"d" * (1472 - 9)

floods = {}  # addr -> stop event


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def flood(sock, addr, secs, stop):
    sent, t0 = 0, time.time()
    while not stop.is_set() and time.time() - t0 < secs:
        try:
            sock.sendto(CHUNK, addr)
            sent += len(CHUNK)
        except OSError:
            time.sleep(0.002)  # device queue full
            continue
        time.sleep(0.003)  # ~450 KB/s cap, well above BT 1.2
    log(f"udp DL to {addr}: {sent} bytes in {time.time() - t0:.1f} s")


def udp_server():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((HOST, UDP_PORT))
    log(f"udp speedtest on {HOST}:{UDP_PORT}")
    ul_bytes, ul_t0, ul_last = 0, 0.0, 0.0
    while True:
        data, addr = sock.recvfrom(4096)
        if data.startswith(b"BTPAN DL"):
            try:
                secs = min(float(data.split()[2]), 30.0)
            except (IndexError, ValueError):
                secs = 8.0
            if addr in floods:
                continue  # request repeated while flooding
            stop = threading.Event()
            floods[addr] = stop

            def run(a=addr, s=secs, e=stop):
                flood(sock, a, s, e)
                floods.pop(a, None)

            threading.Thread(target=run, daemon=True).start()
        elif data.startswith(b"BTPAN STOP"):
            if addr in floods:
                floods[addr].set()
        else:
            now = time.time()
            if now - ul_last > 2:
                if ul_bytes:
                    log(f"udp UL: {ul_bytes} bytes, {ul_bytes / max(ul_last - ul_t0, 0.001) / 1000:.1f} KB/s")
                ul_bytes, ul_t0 = 0, now
            ul_bytes += len(data)
            ul_last = now


def tcp_client(conn, addr):
    conn.settimeout(30)
    try:
        cmd = conn.recv(16)
        t0, n = time.time(), 0
        if cmd.startswith(b"DL"):
            buf = b"d" * 1460
            try:
                while time.time() - t0 < 30:
                    conn.sendall(buf)
                    n += len(buf)
            except OSError:
                pass  # the phone closes when its 6 s are up
        elif cmd.startswith(b"UL"):
            while True:
                d = conn.recv(65536)
                if not d:
                    break
                n += len(d)
        log(f"tcp {cmd[:2].decode(errors='replace')} {addr}: {n} bytes, {n / max(time.time() - t0, 0.001) / 1000:.1f} KB/s")
    except OSError:
        log(f"tcp {addr}: closed")
    finally:
        conn.close()


def tcp_server():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, TCP_PORT))
    srv.listen(4)
    log(f"tcp speedtest on {HOST}:{TCP_PORT}")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=tcp_client, args=(conn, addr), daemon=True).start()


threading.Thread(target=udp_server, daemon=True).start()
tcp_server()
