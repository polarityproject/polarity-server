#!/usr/bin/env python3
# sfvr-reports: takes desync and crash reports from SFVR games (the mod
# sends them unless the player turned it off). Desync reports are filed by
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
# each, at most --per-hour files per address an hour; past --max-total-mb
# (--max-crashes-mb) the oldest folders are removed. GET /v1/health answers
# "ok".
#
# Settings come from /etc/sfvr-server/server.env (sfvr-reports.service):
# SFVR_REPORTS_PORT, SFVR_REPORTS_MAX_MB, SFVR_CRASHES_MAX_MB. Standard
# library only.
import argparse
import collections
import http.server
import os
import re
import shutil
import sys
import threading
import time

NAME = re.compile(
    r"^\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}_[A-Za-z0-9-]{1,40}_vs_[A-Za-z0-9-]{1,40}"
    r"_(?P<match>[0-9a-f]{8})_(?:p1|p2|spec)(?:_desync)?\.(?P<ext>sfvr|log)$"
)
CRASH = re.compile(r"^(?P<day>\d{4}-\d{2}-\d{2})_\d{2}-\d{2}-\d{2}_crash_[0-9a-f]{8}\.(?P<ext>log|dmp)$")
# Recordings and logs start with "SFVR" (a recording with its format
# version, a log with "SFVR <mod version> ..."); a minidump with "MDMP".
MAGIC = {"sfvr": b"SFVR", "log": b"SFVR", "dmp": b"MDMP"}


class Store:
    def __init__(self, root, max_total):
        self.root = root
        self.max_total = max_total
        self.lock = threading.Lock()
        os.makedirs(root, exist_ok=True)
        self.total = sum(self._size(os.path.join(root, d)) for d in os.listdir(root))

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

    def put(self, match, name, data, version):
        """True: stored now; False: already there (a retry)."""
        folder = os.path.join(self.root, match)
        path = os.path.join(folder, name)
        with self.lock:
            if os.path.exists(path) and os.path.getsize(path) == len(data):
                return False
            os.makedirs(folder, mode=0o755, exist_ok=True)
            temp = path + ".part"
            with open(temp, "wb") as f:
                f.write(data)
            os.chmod(temp, 0o644)
            os.replace(temp, path)
            with open(os.path.join(folder, "received.txt"), "a", encoding="utf-8") as f:
                f.write(f"{time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime())} UTC  {version}  {name}\n")
            os.utime(folder)
            self.total += len(data)
            self._prune(keep=match)
            return True

    def _prune(self, keep):
        # The oldest matches go first (a folder's time is its last upload).
        if self.total <= self.max_total:
            return
        folders = sorted(
            (os.path.getmtime(os.path.join(self.root, d)), d)
            for d in os.listdir(self.root)
            if d != keep and os.path.isdir(os.path.join(self.root, d))
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
    def __init__(self, per_hour):
        self.per_hour = per_hour
        self.seen = collections.defaultdict(collections.deque)
        self.lock = threading.Lock()

    def allow(self, address):
        now = time.monotonic()
        with self.lock:
            times = self.seen[address]
            while times and now - times[0] > 3600:
                times.popleft()
            if len(times) >= self.per_hour:
                return False
            times.append(now)
            if len(self.seen) > 10000:  # forget addresses quiet for an hour
                for a in [a for a, t in self.seen.items() if not t or now - t[-1] > 3600]:
                    del self.seen[a]
            return True


def log(text):
    print(text, flush=True)


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "sfvr-reports"
    sys_version = ""
    timeout = 30

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
        name = self.path[len(prefix):]
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
        if length < len(magic):
            return self.answer(400, "empty")
        if not self.server.limiter.allow(self.client_address[0]):
            return self.answer(429, "too many reports, try later")
        data = self.rfile.read(length)
        if len(data) != length:
            return self.answer(400, "cut short")
        if not data.startswith(magic):
            return self.answer(400, "not an SFVR file")
        version = re.sub(r"[^0-9A-Za-z.+-]", "", self.headers.get("X-SFVR-Version", ""))[:32] or "?"
        try:
            stored = store.put(m[key], name, data, version)
        except OSError as e:
            log(f"cannot store {name}: {e}")
            return self.answer(507, "cannot store it")
        if stored:
            log(f"stored {name} ({length // 1024} KB, SFVR {version})")
        self.answer(201 if stored else 200, "stored" if stored else "already here")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", type=int, default=int(os.environ.get("SFVR_REPORTS_PORT", "5580")))
    p.add_argument("--bind", default="0.0.0.0")
    p.add_argument("--dir", default="/var/lib/sfvr-server/reports")
    p.add_argument("--crashes-dir", default="/var/lib/sfvr-server/crashes")
    p.add_argument("--max-total-mb", type=int, default=int(os.environ.get("SFVR_REPORTS_MAX_MB", "4096")))
    p.add_argument("--max-crashes-mb", type=int, default=int(os.environ.get("SFVR_CRASHES_MAX_MB", "1024")))
    p.add_argument("--max-file-mb", type=int, default=16)
    p.add_argument("--per-hour", type=int, default=120)
    a = p.parse_args()
    server = http.server.ThreadingHTTPServer((a.bind, a.port), Handler)
    server.daemon_threads = True
    server.store = Store(a.dir, a.max_total_mb * 1024 * 1024)
    server.crashes = Store(a.crashes_dir, a.max_crashes_mb * 1024 * 1024)
    server.limiter = Limiter(a.per_hour)
    server.max_file = a.max_file_mb * 1024 * 1024
    log(
        f"sfvr-reports on {a.bind}:{a.port}, into {a.dir} ({server.store.total // (1024 * 1024)} of {a.max_total_mb} MB)"
        f" and {a.crashes_dir} ({server.crashes.total // (1024 * 1024)} of {a.max_crashes_mb} MB)"
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
