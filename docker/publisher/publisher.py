#!/usr/bin/env python3
# sfvr-rooms publisher: sends the keeper's public room list (its --status
# file, written every 5 s) to the rooms API (cloudflare/rooms-api), so sites
# and bots read it from Cloudflare and never load the VPS.
#
# Every --every seconds: when the list changed, or --heartbeat seconds went
# by without a push, POST <ROOMS_API_URL>/v1/push with the file as the body
# and "Authorization: Bearer <secret>". Its own counters on
# http://127.0.0.1:9465/metrics (Prometheus). Standard library only.
#
# Settings (environment): ROOMS_API_URL (empty: idle, nothing sent),
# PUSH_SECRET_FILE, STATUS_FILE, PUBLISHER_METRICS (host:port).
import http.server
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request

EVERY = float(os.environ.get("PUBLISH_EVERY", "10"))
# Well under the rooms API's 60 s stale mark (a heartbeat late by one look,
# plus the 10 s cache, must still land before it).
HEARTBEAT = float(os.environ.get("PUBLISH_HEARTBEAT", "30"))
MAX_BYTES = 512 * 1024

stats = {"pushes": 0, "failures": 0, "skipped": 0, "last_success": 0.0, "last_bytes": 0, "rooms": 0}
lock = threading.Lock()


def log(text):
    print(time.strftime("%H:%M:%S ") + text, flush=True)


class Metrics(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/metrics":
            self.send_error(404)
            return
        with lock:
            s = dict(stats)
        body = (
            "# HELP sfvr_publisher_pushes_total Room lists sent to the rooms API.\n# TYPE sfvr_publisher_pushes_total counter\n"
            f"sfvr_publisher_pushes_total {s['pushes']}\n"
            "# HELP sfvr_publisher_failures_total Pushes that failed.\n# TYPE sfvr_publisher_failures_total counter\n"
            f"sfvr_publisher_failures_total {s['failures']}\n"
            "# HELP sfvr_publisher_skipped_total Status files not sent (missing, unreadable, too big, not JSON).\n# TYPE sfvr_publisher_skipped_total counter\n"
            f"sfvr_publisher_skipped_total {s['skipped']}\n"
            "# HELP sfvr_publisher_last_success_seconds When a push last succeeded (Unix time; 0 never).\n# TYPE sfvr_publisher_last_success_seconds gauge\n"
            f"sfvr_publisher_last_success_seconds {s['last_success']:.3f}\n"
            "# HELP sfvr_publisher_last_bytes The last list's size.\n# TYPE sfvr_publisher_last_bytes gauge\n"
            f"sfvr_publisher_last_bytes {s['last_bytes']}\n"
            "# HELP sfvr_publisher_rooms Public rooms in the last list.\n# TYPE sfvr_publisher_rooms gauge\n"
            f"sfvr_publisher_rooms {s['rooms']}\n"
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def read_status(path):
    """The file's bytes and what changes in it (all but `updated`), or None."""
    try:
        with open(path, "rb") as f:
            raw = f.read(MAX_BYTES + 1)
    except OSError:
        return None
    if len(raw) > MAX_BYTES:
        return None
    try:
        doc = json.loads(raw)
    except ValueError:
        return None
    if not isinstance(doc, dict) or doc.get("version") != 1 or not isinstance(doc.get("rooms"), list):
        return None
    key = json.dumps({"totals": doc.get("totals"), "rooms": doc.get("rooms")}, sort_keys=True)
    return raw, key, len(doc["rooms"])


def push(url, secret, body):
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "Content-Type": "application/json",
        "Authorization": "Bearer " + secret,
        "User-Agent": "sfvr-rooms-publisher/1",
    })
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status


def main():
    url = os.environ.get("ROOMS_API_URL", "").rstrip("/")
    status_path = os.environ.get("STATUS_FILE", "/keeper/status.json")
    secret = ""
    secret_path = os.environ.get("PUSH_SECRET_FILE", "")
    if secret_path:
        try:
            with open(secret_path, encoding="utf-8") as f:
                secret = f.read().strip()
        except OSError as e:
            log(f"publisher: cannot read the push secret ({e}); idle")
            url = ""
    host, _, port = os.environ.get("PUBLISHER_METRICS", "127.0.0.1:9465").rpartition(":")
    server = http.server.ThreadingHTTPServer((host or "127.0.0.1", int(port)), Metrics)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    if not url:
        log("publisher: no ROOMS_API_URL; idle (metrics only)")
    else:
        log(f"publisher: {status_path} -> {url}/v1/push every {EVERY:g} s (at least every {HEARTBEAT:g} s)")
    last_key, last_push, failing_since, said_missing = None, 0.0, None, False
    while True:
        time.sleep(EVERY)
        if not url:
            continue
        got = read_status(status_path)
        if got is None:
            with lock:
                stats["skipped"] += 1
            if not said_missing:
                log(f"publisher: {status_path} is missing or not a room list; waiting")
                said_missing = True
            continue
        said_missing = False
        raw, key, rooms = got
        now = time.time()
        if key == last_key and now - last_push < HEARTBEAT:
            continue
        try:
            code = push(url + "/v1/push", secret, raw)
            ok = 200 <= code < 300
            why = f"HTTP {code}"
        except urllib.error.HTTPError as e:
            ok, why = False, f"HTTP {e.code}"
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            ok, why = False, str(getattr(e, "reason", e))
        with lock:
            if ok:
                stats["pushes"] += 1
                stats["last_success"] = now
                stats["last_bytes"] = len(raw)
                stats["rooms"] = rooms
            else:
                stats["failures"] += 1
        if ok:
            last_key, last_push = key, now
            if failing_since is not None:
                log(f"publisher: pushing again after {now - failing_since:.0f} s of failures")
                failing_since = None
        elif failing_since is None:
            failing_since = now
            log(f"publisher: push failed ({why}); trying every {EVERY:g} s")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
