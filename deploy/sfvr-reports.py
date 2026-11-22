#!/usr/bin/env python3
# polarity-reports: takes desync and crash reports from Polarity games (the
# mod sends them unless the player turned it off). Installed on the VPS
# under its deployment name, sfvr-reports. Desync reports are filed by
# match, so both players' halves of a desync end up side by side; crash
# reports by day:
#
#   <reports dir>/<match id>/<file name as the game saved it>
#   <crashes dir>/<date>/<date>_<time>_crash_<id>.log (and .dmp)
#
# One file per request, the file as the body: POST /v1/reports/<file name>
# (recordings and match logs: .sfvr, .log) or POST /v1/crashes/<file name>
# (a crash's log and minidump: .log, .dmp). Only the names the mod gives
# them, only files that start as the mod writes them, at most --max-file-mb
# each, at most --per-hour files and --per-day-mb an address; past
# --max-total-mb (--max-crashes-mb) the oldest folders are removed. A stored
# file is never replaced: the same name again is a retry (same size) or
# refused (409). GET /v1/health answers "ok".
#
# Bodies go to disk in chunks. At most --max-uploads in progress
# (--max-uploads-per-address from one address), --max-connections open, and
# each connection closed after --deadline seconds.
#
# Settings come from the environment (docker/compose.yaml; the deployment
# keeps its SFVR names):
# SFVR_REPORTS_PORT, SFVR_REPORTS_MAX_MB, SFVR_CRASHES_MAX_MB. Standard
# library only.
import argparse
import collections
import http.server
import os
import re
import shutil
import socket
import sys
import threading
import time

NAME = re.compile(
    r"^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_[A-Za-z0-9-]{1,40}_vs_[A-Za-z0-9-]{1,40}"
    r"_(?P<match>[0-9a-f]{8})_(?:p1|p2|spec)(?:_desync)?\.(?P<ext>sfvr|log)$"
)
CRASH = re.compile(r"^(?P<day>\d{4}-\d{2}-\d{2})_\d{2}-\d{2}-\d{2}_crash_[0-9a-f]{8}\.(?P<ext>log|dmp)$")
# Recordings start with "SFVR" and their format version (the magic stays
# with the rename); logs with "Polarity <mod version> ...", or "SFVR <mod
# version> ..." from builds before 0.4.0; a minidump with "MDMP".
MAGIC = {"sfvr": (b"SFVR",), "log": (b"Polarity", b"SFVR"), "dmp": (b"MDMP",)}
CHUNK = 64 * 1024
INCOMING = ".incoming"


class Store:
    def __init__(self, root, max_total):
        self.root = root
        self.max_total = max_total
        self.lock = threading.Lock()
        os.makedirs(root, exist_ok=True)
        # Uploads in progress; on the same file system, so a finished one moves.
        self.incoming = os.path.join(root, INCOMING)
        shutil.rmtree(self.incoming, ignore_errors=True)
        os.makedirs(self.incoming, mode=0o700)
        self.total = sum(self._size(os.path.join(root, d)) for d in os.listdir(root) if not d.startswith("."))

    @staticmethod
    def _size(path):
        if os.path.isfile(path):
            return os.path.getsize(path)
        total = 0
        for base, _, files in os.walk(path):
            for f in files:
                try:
                    total += os.path.getsize(os.path.join(base, f))
                except OSError:
                    pass
        return total

    def temp_path(self):
        return os.path.join(self.incoming, f"{threading.get_ident()}.part")

    def put(self, match, name, temp, length, version):
        """Files the upload at temp: "stored", "retry" (the same file is already
        here) or "taken" (another file has the name; never replaced)."""
        folder = os.path.join(self.root, match)
        path = os.path.join(folder, name)
        with self.lock:
            if os.path.exists(path):
                os.remove(temp)
                return "retry" if os.path.getsize(path) == length else "taken"
            os.makedirs(folder, mode=0o755, exist_ok=True)
            os.chmod(temp, 0o644)
            os.replace(temp, path)
            with open(os.path.join(folder, "received.txt"), "a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime())} UTC  {version}  {name}\n")
            os.utime(folder)
            self.total += length
            self._prune(keep=match)
            return "stored"

    def _prune(self, keep):
        # The oldest matches go first (a folder's time is its last upload).
        if self.total <= self.max_total:
            return
        # Counted again first: collect-reports.py deletes folders behind our
        # back, and its ".collecting-*" folder (being packed) is not ours.
        self.total = sum(self._size(os.path.join(self.root, d)) for d in os.listdir(self.root) if not d.startswith("."))
        folders = sorted(
            (os.path.getmtime(os.path.join(self.root, d)), d)
            for d in os.listdir(self.root)
            if d != keep and not d.startswith(".") and os.path.isdir(os.path.join(self.root, d))
        )
        for _, d in folders:
            if self.total <= self.max_total:
                break
            path = os.path.join(self.root, d)
            size = self._size(path)
            shutil.rmtree(path, ignore_errors=True)
            self.total -= size
            log(f"removed {d} ({size // 1024} KB): over {self.max_total // (1024 * 1024)} MB")


class Limiter:
    """Files an hour and bytes a day per address. An upload's bytes count
    from its start (by Content-Length) and are given back when it is not
    stored."""

    def __init__(self, per_hour, per_day_bytes):
        self.per_hour = per_hour
        self.per_day = per_day_bytes
        self.files = collections.defaultdict(collections.deque)
        self.bytes = collections.defaultdict(collections.deque)
        self.lock = threading.Lock()

    def allow(self, address, length):
        now = time.monotonic()
        with self.lock:
            files, sent = self.files[address], self.bytes[address]
            while files and now - files[0] > 3600:
                files.popleft()
            while sent and now - sent[0][0] > 86400:
                sent.popleft()
            if len(files) >= self.per_hour or sum(n for _, n in sent) + length > self.per_day:
                return False
            files.append(now)
            sent.append((now, length))
            if len(self.bytes) > 10000:  # forget addresses quiet for a day
                for a in [a for a, s in self.bytes.items() if not s or now - s[-1][0] > 86400]:
                    del self.bytes[a]
                    self.files.pop(a, None)
            return True

    def give_back(self, address, length):
        with self.lock:
            self.bytes[address].append((time.monotonic(), -length))


class Gate:
    """Uploads in progress, in all and per address."""

    def __init__(self, total, per_address):
        self.total = total
        self.per_address = per_address
        self.open = collections.Counter()
        self.lock = threading.Lock()

    def enter(self, address):
        """None when let in, else the refusal (status, text)."""
        with self.lock:
            if sum(self.open.values()) >= self.total:
                return 503, "busy, try later"
            if self.open[address] >= self.per_address:
                return 429, "too many uploads at once"
            self.open[address] += 1
            return None

    def leave(self, address):
        with self.lock:
            self.open[address] -= 1
            if self.open[address] <= 0:
                del self.open[address]


def log(text):
    print(text, flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "polarity-reports"
    sys_version = ""
    timeout = 30

    def setup(self):
        super().setup()
        # One request per connection (HTTP/1.0), so this is its whole time.
        self.deadline = threading.Timer(self.server.deadline, self.expire)
        self.deadline.daemon = True
        self.deadline.start()

    def finish(self):
        self.deadline.cancel()
        super().finish()

    def expire(self):
        log(f"closed a connection after {self.server.deadline} s")
        try:
            self.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    def log_message(self, fmt, *args):
        pass  # one line per upload instead (no addresses in the journal)

    def answer(self, status, text):
        body = (text + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/v1/health":
            return self.answer(200, "ok")
        self.answer(404, "not found")

    def do_POST(self):
        for prefix, pattern, store, key in (
            ("/v1/reports/", NAME, self.server.store, "match"),
            ("/v1/crashes/", CRASH, self.server.crashes, "day"),
        ):
            if self.path.startswith(prefix):
                break
        else:
            return self.answer(404, "not found")
        name = self.path[len(prefix) :]
        m = pattern.match(name)
        if not m:
            return self.answer(400, "not a report name")
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            return self.answer(411, "length required")
        if length > self.server.max_file:
            return self.answer(413, "too large")
        magic = MAGIC[m["ext"]]
        if length < min(len(x) for x in magic):
            return self.answer(400, "empty")
        address = self.client_address[0]
        refused = self.server.gate.enter(address)
        if refused:
            return self.answer(*refused)
        try:
            if not self.server.limiter.allow(address, length):
                return self.answer(429, "too many reports, try later")
            status, text = self.receive(store, m[key], name, length, magic)
            if status != 201:
                self.server.limiter.give_back(address, length)
            self.answer(status, text)
        finally:
            self.server.gate.leave(address)

    def receive(self, store, folder, name, length, magic):
        temp = store.temp_path()
        try:
            with open(temp, "wb") as f:
                left = length
                while left:
                    try:
                        chunk = self.rfile.read(min(CHUNK, left))
                    except OSError:  # a read timeout, a reset, or the deadline
                        chunk = b""
                    if not chunk:
                        return 400, "cut short"
                    if left == length and not chunk.startswith(magic):
                        return 400, "not a Polarity file"
                    f.write(chunk)
                    left -= len(chunk)
            # X-SFVR-Version from builds before 0.4.0.
            sent = self.headers.get("X-Polarity-Version") or self.headers.get("X-SFVR-Version", "")
            version = re.sub(r"[^0-9A-Za-z.+-]", "", sent)[:32] or "?"
            result = store.put(folder, name, temp, length, version)
        except OSError as e:
            log(f"cannot store {name}: {type(e).__name__}")
            return 507, "cannot store it"
        finally:
            if os.path.exists(temp):
                os.remove(temp)
        if result == "stored":
            log(f"stored {name} ({length // 1024} KB, version {version})")
            return 201, "stored"
        if result == "taken":
            log(f"refused {name}: another file has that name")
            return 409, "another file has that name"
        return 200, "already here"


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, max_connections):
        super().__init__(address, Handler)
        self.connections = threading.BoundedSemaphore(max_connections)

    def process_request(self, request, client_address):
        if not self.connections.acquire(blocking=False):
            self.shutdown_request(request)  # full: closed unanswered
            return
        super().process_request(request, client_address)

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.connections.release()

    def handle_error(self, request, client_address):
        # Without the address or the traceback (resets while sending are normal).
        log(f"a connection failed: {sys.exc_info()[0].__name__}")


def parse(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, default=int(os.environ.get("SFVR_REPORTS_PORT", "5580")))
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--dir", default="/var/lib/sfvr-server/reports")
    p.add_argument("--crashes-dir", default="/var/lib/sfvr-server/crashes")
    p.add_argument("--max-total-mb", type=int, default=int(os.environ.get("SFVR_REPORTS_MAX_MB", "4096")))
    p.add_argument("--max-crashes-mb", type=int, default=int(os.environ.get("SFVR_CRASHES_MAX_MB", "1024")))
    p.add_argument("--max-file-mb", type=int, default=16)
    p.add_argument("--per-hour", type=int, default=120)
    p.add_argument("--per-day-mb", type=int, default=256)
    p.add_argument("--max-uploads", type=int, default=16)
    p.add_argument("--max-uploads-per-address", type=int, default=2)
    p.add_argument("--max-connections", type=int, default=64)
    p.add_argument("--deadline", type=float, default=300)
    return p.parse_args(argv)


def build(a):
    server = Server((a.bind, a.port), a.max_connections)
    server.store = Store(a.dir, a.max_total_mb * 1024 * 1024)
    server.crashes = Store(a.crashes_dir, a.max_crashes_mb * 1024 * 1024)
    server.limiter = Limiter(a.per_hour, a.per_day_mb * 1024 * 1024)
    server.gate = Gate(a.max_uploads, a.max_uploads_per_address)
    server.max_file = a.max_file_mb * 1024 * 1024
    server.deadline = a.deadline
    return server


def main():
    a = parse()
    server = build(a)
    log(
        f"polarity-reports on {a.bind}:{a.port}, into {a.dir} ({server.store.total // (1024 * 1024)} of {a.max_total_mb} MB)"
        f" and {a.crashes_dir} ({server.crashes.total // (1024 * 1024)} of {a.max_crashes_mb} MB)"
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
