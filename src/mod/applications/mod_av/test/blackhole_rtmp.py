#!/usr/bin/env python3
"""Stand-in for an overloaded RTMP recorder, for manual/scenario testing.

The unit tests in test_avformat.c cover an *unreachable* recorder, which is
deterministic and needs no server.  This covers the other shape, and the one
that actually caused the incident: a recorder that completes the TCP connect
and then never answers.  The kernel accepts into the backlog, so the client
sees a healthy socket and waits on a handshake that never comes.

  accept  - accept the connection, never answer the RTMP handshake
  silent  - answer the handshake, then stop reading, so writes block once the
            socket buffers fill

Usage: blackhole_rtmp.py <port> [accept|silent]

See test/README.stall-testing.md for how to drive a call against it.
"""
import socket
import sys
import threading


def handle(conn, addr, mode):
    print(f"  [server] connection from {addr[0]}:{addr[1]} ({mode})", flush=True)
    if mode == "silent":
        try:
            conn.settimeout(5)
            if conn.recv(1537):
                conn.sendall(b"\x03" + b"\x00" * 1536)  # S0 + S1
        except Exception:
            pass
    try:
        while True:
            if not conn.recv(65536):
                break
    except Exception:
        pass
    finally:
        conn.close()
        print(f"  [server] {addr[0]}:{addr[1]} closed", flush=True)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 1935
    mode = sys.argv[2] if len(sys.argv) > 2 else "accept"

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    print(f"[server] black-hole RTMP on 127.0.0.1:{port} mode={mode}", flush=True)

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr, mode), daemon=True).start()


if __name__ == "__main__":
    main()
