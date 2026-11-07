#!/usr/bin/env python3
# sfvr-rooms container-stats: CPU, memory, state and restarts of the stack's
# containers, from the Docker API (its socket, read-only), for Prometheus on
# http://127.0.0.1:9466/metrics. Stands in for cAdvisor, which cannot place
# containers when Docker keeps images in containerd (Docker 29's default:
# "failed to identify the read-write layer ID"). Standard library only.
#
#   sfvr_container_up{service,name}                  1 running, 0 not
#   sfvr_container_healthy{service,name}             1 its health check passes, 0 not (those with one)
#   sfvr_container_restarts{service,name}            Docker's restart count
#   sfvr_container_cpu_seconds_total{service,name}   CPU used since it started
#   sfvr_container_memory_bytes{service,name}        working set (usage less inactive files)
#   sfvr_container_memory_limit_bytes{service,name}
#   sfvr_container_pids{service,name}
import http.client
import http.server
import json
import os
import socket
import threading
import time

SOCKET = os.environ.get("DOCKER_SOCKET", "/var/run/docker.sock")
LISTEN = os.environ.get("CONTAINER_STATS_LISTEN", "127.0.0.1:9466")
# Only this compose project's containers (others on the machine, and
# one-off runs, are not the stack's).
PROJECT = os.environ.get("COMPOSE_PROJECT", "sfvr-rooms")
CACHE_SECONDS = 5


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=10)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect(self.path)


def api(path):
    c = UnixConnection(SOCKET)
    try:
        c.request("GET", path)
        r = c.getresponse()
        body = r.read()
        if r.status != 200:
            raise OSError(f"Docker API {path}: HTTP {r.status}")
        return json.loads(body)
    finally:
        c.close()


def esc(v):
    return str(v).replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ")


def collect():
    lines = []
    def metric(name, kind, help_text, rows):
        lines.append(f"# HELP {name} {help_text}\n# TYPE {name} {kind}")
        for labels, value in rows:
            lines.append(f'{name}{{service="{esc(labels[0])}",name="{esc(labels[1])}"}} {value}')

    up, restarts, cpu, mem, limit, pids, healthy = [], [], [], [], [], [], []
    ok = 1
    try:
        for c in api("/containers/json?all=true"):
            labels = c.get("Labels") or {}
            if labels.get("com.docker.compose.project") != PROJECT or labels.get("com.docker.compose.oneoff") == "True":
                continue
            service = labels.get("com.docker.compose.service", "")
            name = (c.get("Names") or ["?"])[0].lstrip("/")
            key = (service, name)
            running = c.get("State") == "running"
            up.append((key, 1 if running else 0))
            try:
                inspect = api(f"/containers/{c['Id']}/json")
                restarts.append((key, inspect.get("RestartCount", 0)))
                health = (inspect.get("State") or {}).get("Health")
                if health:
                    healthy.append((key, 1 if health.get("Status") == "healthy" else 0))
            except (OSError, ValueError):
                pass
            if not running:
                continue
            try:
                s = api(f"/containers/{c['Id']}/stats?stream=false&one-shot=true")
            except (OSError, ValueError):
                continue
            cpu.append((key, s.get("cpu_stats", {}).get("cpu_usage", {}).get("total_usage", 0) / 1e9))
            m = s.get("memory_stats", {})
            inactive = m.get("stats", {}).get("inactive_file", m.get("stats", {}).get("total_inactive_file", 0))
            mem.append((key, max(0, m.get("usage", 0) - inactive)))
            limit.append((key, m.get("limit", 0)))
            pids.append((key, s.get("pids_stats", {}).get("current", 0)))
    except (OSError, ValueError) as e:
        ok = 0
        print(time.strftime("%H:%M:%S ") + f"container-stats: {e}", flush=True)
    metric("sfvr_container_up", "gauge", "1 when the container is running.", up)
    metric("sfvr_container_healthy", "gauge", "1 when the container's health check passes (only containers that have one).", healthy)
    metric("sfvr_container_restarts", "gauge", "Times Docker restarted the container.", restarts)
    metric("sfvr_container_cpu_seconds_total", "counter", "CPU time the container used since it started.", cpu)
    metric("sfvr_container_memory_bytes", "gauge", "Memory in use, less inactive file cache (the working set).", mem)
    metric("sfvr_container_memory_limit_bytes", "gauge", "The container's memory limit.", limit)
    metric("sfvr_container_pids", "gauge", "Processes and threads in the container.", pids)
    lines.append("# HELP sfvr_container_stats_up 1 when the Docker API answered.\n# TYPE sfvr_container_stats_up gauge")
    lines.append(f"sfvr_container_stats_up {ok}")
    return ("\n".join(lines) + "\n").encode()


cache = {"at": 0.0, "body": b""}
lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/metrics":
            self.send_error(404)
            return
        with lock:
            if time.time() - cache["at"] > CACHE_SECONDS:
                cache["body"], cache["at"] = collect(), time.time()
            body = cache["body"]
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; version=0.0.4")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    host, _, port = LISTEN.rpartition(":")
    print(time.strftime("%H:%M:%S ") + f"container-stats: {SOCKET} on {LISTEN}", flush=True)
    http.server.ThreadingHTTPServer((host, int(port)), Handler).serve_forever()
