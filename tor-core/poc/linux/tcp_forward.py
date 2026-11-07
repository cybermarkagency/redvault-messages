#!/usr/bin/env python3
"""tcp_forward.py <listen-ip> <listen-port> <target-ip> <target-port>: relays TCP, for the netns lab's access to the
Windows-side API (the namespace "cli" can only reach this machine's root namespace)."""
import socket, sys, threading
lip, lport, tip, tport = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
def pump(a, b):
    try:
        while (d := a.recv(65536)):
            b.sendall(d)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind((lip, lport)); srv.listen(64)
while True:
    c, _ = srv.accept()
    try: u = socket.create_connection((tip, tport), timeout=10)
    except OSError: c.close(); continue
    threading.Thread(target=pump, args=(c, u), daemon=True).start(); threading.Thread(target=pump, args=(u, c), daemon=True).start()
