#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

"""Stands in for the API's onion listener (127.0.0.1:3001): answers every request with the path and headers it received,
and for PUT/POST counts and hashes the body as it streams past (nothing is stored)."""
import hashlib
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Echo(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle_any(self):
        received, digest = 0, hashlib.sha256()
        left = int(self.headers.get("Content-Length") or 0)
        while left > 0:
            chunk = self.rfile.read(min(left, 1 << 20))
            if not chunk:
                break
            received += len(chunk)
            digest.update(chunk)
            left -= len(chunk)
        body = json.dumps(
            {
                "method": self.command,
                "path": self.path,
                "headers": {k.lower(): v for k, v in self.headers.items()},
                "bodyBytes": received,
                "bodySha256": digest.hexdigest() if received else None,
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = do_PUT = do_POST = do_DELETE = handle_any

    def log_message(self, *args):
        pass


ThreadingHTTPServer(("127.0.0.1", 3001), Echo).serve_forever()
