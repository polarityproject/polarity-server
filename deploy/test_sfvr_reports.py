"""Tests for sfvr-reports.py against a local instance (no network beyond loopback).
python deploy/test_sfvr_reports.py"""

import http.client
import importlib.util
import os
import socket
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("sfvr_reports", os.path.join(HERE, "sfvr-reports.py"))
reports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reports)
reports.log = lambda text: None

NAME = "2026-11-22_10-00-00_Player-1-RYU_vs_Player-2-KEN_0123abcd_p1_desync.sfvr"
CRASH = "2026-11-22_10-00-00_crash_0123abcd.log"


class Reports(unittest.TestCase):
    def start(self, *args):
        self.dir = tempfile.TemporaryDirectory()
        root = self.dir.name
        a = reports.parse(
            ["--port", "0", "--bind", "127.0.0.1", "--dir", f"{root}/r", "--crashes-dir", f"{root}/c", *args]
        )
        self.server = reports.build(a)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.dir.cleanup()

    def post(self, name, body, prefix="/v1/reports/"):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        c.request("POST", prefix + name, body=body, headers={"X-Polarity-Version": "0.4.4"})
        r = c.getresponse()
        r.read()
        c.close()
        return r.status

    def slow(self, name, length):
        """An upload that sends its headers and a few bytes, then waits."""
        s = socket.create_connection(("127.0.0.1", self.port))
        s.sendall(f"POST /v1/reports/{name} HTTP/1.0\r\nContent-Length: {length}\r\n\r\nSFVR".encode())
        time.sleep(0.2)
        return s

    def head(self, name, length):
        """Headers alone: the answer's status line."""
        s = socket.create_connection(("127.0.0.1", self.port))
        s.settimeout(5)
        s.sendall(f"POST /v1/reports/{name} HTTP/1.0\r\nContent-Length: {length}\r\n\r\n".encode())
        line = s.recv(4096).split(b"\r\n")[0]
        s.close()
        return line

    def idle(self):
        """Waits for the uploads in progress to end."""
        for _ in range(50):
            if not self.server.gate.open:
                return
            time.sleep(0.1)
        self.fail("uploads still open")

    def stored(self, name):
        return os.path.join(self.dir.name, "r", "0123abcd", name)

    def test_stores_and_never_replaces(self):
        self.start()
        body = b"SFVR" + os.urandom(200_000)
        self.assertEqual(self.post(NAME, body), 201)
        self.assertEqual(self.post(NAME, body), 200)  # a retry
        for n in range(2, 6):  # another file under the name: kept beside it
            self.assertEqual(self.post(NAME, b"SFVR" + b"x" * n), 201)
            self.assertTrue(os.path.exists(self.stored(f"{NAME}.{n}")))
        self.assertEqual(self.post(NAME, b"SFVR" + b"x" * 3), 200)  # a retry of name.3
        self.assertEqual(self.post(NAME, b"SFVR" + b"x" * 9), 409)  # past name.5
        with open(self.stored(NAME), "rb") as f:
            self.assertEqual(f.read(), body)
        self.assertEqual(self.post(CRASH, b"Polarity 0.4.4\n", "/v1/crashes/"), 201)

    def test_refuses_bad_uploads(self):
        self.start("--max-file-mb", "1")
        self.assertEqual(self.post(NAME, b"MDMP" + b"x" * 100), 400)
        self.assertEqual(self.post("x.sfvr", b"SFVR"), 400)
        self.assertEqual(self.head(NAME, 1024 * 1024 + 1)[:12], b"HTTP/1.0 413")
        self.assertFalse(os.path.exists(self.stored(NAME)))
        self.assertEqual(os.listdir(os.path.join(self.dir.name, "r", ".incoming")), [])

    def test_uploads_at_once(self):
        self.start("--max-uploads", "3", "--max-uploads-per-address", "2")
        held = [self.slow(NAME.replace("p1", f"p{i}"), 1000) for i in (1, 2)]
        # Refused before the body is read: headers only, as a body left
        # unread can turn the close into a reset before the answer is read.
        self.assertEqual(self.head(NAME, 14)[:12], b"HTTP/1.0 429")
        self.server.gate.per_address = 5
        held.append(self.slow(NAME.replace("p1", "spec"), 1000))
        self.assertEqual(self.head(NAME, 14)[:12], b"HTTP/1.0 503")
        for s in held:
            s.close()
        self.idle()
        self.assertEqual(self.post(NAME, b"SFVR" + b"x" * 10), 201)

    def test_deadline(self):
        self.start("--deadline", "1", "--max-uploads-per-address", "1")
        s = self.slow(NAME, 1000)
        s.settimeout(5)
        try:
            self.assertEqual(s.recv(4096), b"")  # closed at the deadline
        except ConnectionError:
            pass
        s.close()
        self.idle()
        self.assertFalse(os.path.exists(self.stored(NAME)))
        self.assertEqual(self.post(NAME, b"SFVR" + b"x" * 10), 201)

    def test_daily_quota(self):
        self.start("--per-day-mb", "2")
        body = b"SFVR" + os.urandom(600_000)
        self.assertEqual(self.post(NAME, body), 201)
        self.assertEqual(self.post(NAME, body), 200)  # a retry: its bytes are given back
        self.assertEqual(self.post(NAME.replace("p1", "p2"), body), 201)
        self.assertEqual(self.post(NAME.replace("p1", "spec"), body), 201)
        self.assertEqual(self.head(NAME.replace("_desync", ""), len(body))[:12], b"HTTP/1.0 429")

    def test_connections(self):
        self.start("--max-connections", "2")
        idle = [socket.create_connection(("127.0.0.1", self.port)) for _ in range(2)]
        time.sleep(0.2)
        third = socket.create_connection(("127.0.0.1", self.port))
        third.settimeout(5)
        self.assertEqual(third.recv(1), b"")  # closed unanswered
        third.close()
        for s in idle:
            s.close()
        time.sleep(0.3)
        self.assertEqual(self.post(NAME, b"SFVR" + b"x" * 10), 201)


if __name__ == "__main__":
    unittest.main()
