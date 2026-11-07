#!/usr/bin/env python3
# Writes the rooms VPS's Grafana dashboards (monitoring/dashboards/*.json,
# provisioned read-only by compose.monitoring.yaml). Edit here, run it
# (python3 make-dashboards.py), commit both.
#   sfvr-rooms-service.json  the keeper: loop, rooms, tables, sets, relays, the publisher
#   sfvr-rooms-vps.json      the VPS: CPU, memory, disk, network, per container
#   sfvr-rooms-logs.json     the containers' and the host's logs (Loki)
import json
import os

PROM = {"type": "prometheus", "uid": "prometheus"}
LOKI = {"type": "loki", "uid": "loki"}
HERE = os.path.dirname(os.path.abspath(__file__))


class Board:
    def __init__(self, uid, title, description, refresh="30s", time_from="now-6h"):
        self.uid, self.title, self.description = uid, title, description
        self.refresh, self.time_from = refresh, time_from
        self.panels, self.templating = [], []
        self.x = self.y = self.row_h = 0
        self.next_id = 1

    def _place(self, w, h):
        if self.x + w > 24:
            self.x, self.y = 0, self.y + self.row_h
            self.row_h = 0
        pos = {"x": self.x, "y": self.y, "w": w, "h": h}
        self.x += w
        self.row_h = max(self.row_h, h)
        return pos

    def row(self, title):
        if self.x:
            self.x, self.y = 0, self.y + self.row_h
        self.row_h = 0
        self.panels.append({"id": self._id(), "type": "row", "title": title, "collapsed": False,
                            "gridPos": {"x": 0, "y": self.y, "w": 24, "h": 1}, "panels": []})
        self.y += 1

    def _id(self):
        self.next_id += 1
        return self.next_id - 1

    def stat(self, title, expr, unit="short", w=4, h=4, description="", thresholds=None, decimals=None, mappings=None):
        defaults = {"unit": unit, "thresholds": {"mode": "absolute", "steps": thresholds or [{"color": "green", "value": None}]}}
        if decimals is not None:
            defaults["decimals"] = decimals
        if mappings:
            defaults["mappings"] = mappings
        self.panels.append({
            "id": self._id(), "type": "stat", "title": title, "description": description, "datasource": PROM,
            "gridPos": self._place(w, h),
            "targets": [{"refId": "A", "datasource": PROM, "expr": expr, "instant": True}],
            "fieldConfig": {"defaults": defaults, "overrides": []},
            "options": {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
                        "colorMode": "value", "graphMode": "area", "textMode": "auto", "justifyMode": "auto"},
        })

    def series(self, title, targets, unit="short", w=12, h=8, description="", stack=False, min0=True):
        """targets: [(expr, legend)]"""
        custom = {"drawStyle": "line", "lineWidth": 1, "fillOpacity": 25 if stack else 8, "showPoints": "never",
                  "spanNulls": True, "stacking": {"mode": "normal" if stack else "none", "group": "A"}}
        defaults = {"unit": unit, "custom": custom}
        if min0:
            defaults["min"] = 0
        self.panels.append({
            "id": self._id(), "type": "timeseries", "title": title, "description": description, "datasource": PROM,
            "gridPos": self._place(w, h),
            "targets": [{"refId": chr(65 + i), "datasource": PROM, "expr": e, "legendFormat": l} for i, (e, l) in enumerate(targets)],
            "fieldConfig": {"defaults": defaults, "overrides": []},
            "options": {"legend": {"displayMode": "list", "placement": "bottom"}, "tooltip": {"mode": "multi", "sort": "desc"}},
        })

    def logs(self, title, expr, w=24, h=10, description=""):
        self.panels.append({
            "id": self._id(), "type": "logs", "title": title, "description": description, "datasource": LOKI,
            "gridPos": self._place(w, h),
            "targets": [{"refId": "A", "datasource": LOKI, "expr": expr}],
            "options": {"showTime": True, "wrapLogMessage": True, "sortOrder": "Descending", "enableLogDetails": True,
                        "dedupStrategy": "none", "prettifyLogMessage": False},
        })

    def log_series(self, title, targets, w=24, h=6, description=""):
        self.panels.append({
            "id": self._id(), "type": "timeseries", "title": title, "description": description, "datasource": LOKI,
            "gridPos": self._place(w, h),
            "targets": [{"refId": chr(65 + i), "datasource": LOKI, "expr": e, "legendFormat": l} for i, (e, l) in enumerate(targets)],
            "fieldConfig": {"defaults": {"unit": "short", "min": 0, "custom": {"drawStyle": "bars", "fillOpacity": 60, "stacking": {"mode": "normal"}}}, "overrides": []},
            "options": {"legend": {"displayMode": "list", "placement": "bottom"}, "tooltip": {"mode": "multi", "sort": "desc"}},
        })

    def textbox(self, name, label, default=""):
        self.templating.append({"type": "textbox", "name": name, "label": label, "query": default,
                                "current": {"text": default, "value": default}, "hide": 0})

    def write(self, name):
        doc = {
            "uid": self.uid, "title": self.title, "description": self.description, "tags": ["sfvr"],
            "timezone": "browser", "schemaVersion": 39, "version": 1, "editable": False, "graphTooltip": 1,
            "refresh": self.refresh, "time": {"from": self.time_from, "to": "now"},
            "templating": {"list": self.templating}, "annotations": {"list": []},
            "links": [{"title": "SFVR", "type": "dashboards", "tags": ["sfvr"], "asDropdown": True}],
            "panels": self.panels,
        }
        path = os.path.join(HERE, "dashboards", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        print("wrote", path)


RED_AT = lambda warn, crit: [{"color": "green", "value": None}, {"color": "orange", "value": warn}, {"color": "red", "value": crit}]
SVC = 'service'


def service():
    b = Board("sfvr-rooms-service", "SFVR rooms service",
              "The keeper (tables, rooms, relays) and the rooms publisher, from the keeper's metrics file.")
    b.row("Now")
    b.stat("Desk", "sfvr_keeper_lobby_up", w=2,
           description="Whether the keeper hosts the desk (the lobby channel): room requests go through it and its listing carries the public list's summary. Down: players see custom rooms as offline.",
           thresholds=[{"color": "red", "value": None}, {"color": "green", "value": 1}],
           mappings=[{"type": "value", "options": {"0": {"text": "DOWN"}, "1": {"text": "UP"}}}])
    b.stat("Metrics age", 'time() - node_textfile_mtime_seconds{file=~".*keeper.prom"}', unit="s", w=3,
           description="Seconds since the keeper last wrote its metrics (every 5 s; a stuck loop stops it).",
           thresholds=RED_AT(15, 60), decimals=0)
    b.stat("Online", "sfvr_keeper_players_online", w=3,
           description="Players connected to the rooms server: on the hub, at the desk or in a room (the rooms API's online). From the server's connection count; if its stats file is stale, only room members (see Members).")
    b.stat("At the desk", "sfvr_keeper_lobby_members", w=2,
           description="Players with the custom rooms screen open (they join the desk to make rooms and see previews). The hub alone is in no room.")
    b.stat("Rooms", "sfvr_keeper_rooms", w=2)
    b.stat("Public rooms", "sfvr_keeper_public_rooms", w=3)
    b.stat("In rooms", "sfvr_keeper_room_members", w=3)
    b.stat("Tables playing", 'sum(sfvr_keeper_tables{phase=~"connecting|playing"})', w=3)
    b.stat("Watching", "sfvr_keeper_relay_viewers", w=3)

    b.row("The keeper's loop (one thread: when it falls behind, every answer waits)")
    b.series("Step time", [
        ("sfvr_keeper_step_max_seconds", "longest in 5 s"),
        ("histogram_quantile(0.99, rate(sfvr_keeper_step_seconds_bucket[1m]))", "p99"),
        ("histogram_quantile(0.5, rate(sfvr_keeper_step_seconds_bucket[1m]))", "median"),
    ], unit="s", w=12, description="How long one pass over every room takes. Requests wait up to this long.")
    b.series("Loop busy", [
        ("rate(sfvr_keeper_step_seconds_sum[1m])", "stepping"),
        ("rate(sfvr_keeper_log_seconds_total[1m])", "writing log lines"),
        ("rate(sfvr_keeper_connect_seconds_total[1m])", "waiting on server connects"),
        ('rate(sfvr_container_cpu_seconds_total{service="keeper"}[1m])', "keeper CPU"),
    ], unit="percentunit", w=12, description="Share of time the loop is working (1 = never idle), and how much of it is log writing or waiting for a connection to open. Busy with little CPU: it is waiting.")
    b.series("Answers and resends", [
        ("rate(sfvr_keeper_answers_total[1m])", "answers/s"),
        ("rate(sfvr_keeper_notices_total[1m])", "notices/s"),
        ("rate(sfvr_keeper_resends_total[1m])", "resends/s"),
        ("sfvr_keeper_outbox", "waiting for an ack"),
    ], w=12, description="Resends: answers not acknowledged within 250 ms (slow or lost links).")
    b.series("Server connects", [
        ("rate(sfvr_keeper_connects_total[1m]) * 60", "per minute"),
        ("rate(sfvr_keeper_connect_failures_total[1m]) * 60", "failed per minute"),
        ("sfvr_keeper_connects_pending", "waiting now"),
    ], w=6, description="One per room and per table set's relay room, made on worker threads (the loop goes on meanwhile). A firewall limit on them makes each wait (TCP retries): waiting ones pile up and new rooms open late; one not through in 30 s fails.")
    b.series("Longest connect", [("sfvr_keeper_connect_max_seconds", "in 5 s")], unit="s", w=6)
    b.series("Requests by kind", [("sum by (kind) (rate(sfvr_keeper_requests_total[1m]) * 60)", "{{kind}}")],
             unit="short", w=12, stack=True, description="Per minute.")

    b.row("Rooms and tables")
    b.series("Members", [
        ("sfvr_keeper_players_online", "online"),
        ("sfvr_keeper_server_connections", "server connections"),
        ("sfvr_keeper_lobby_members", "at the desk (rooms screen)"),
        ("sfvr_keeper_room_members", "in rooms"),
        ("sfvr_keeper_queued", "queued at tables"),
    ], w=8, description="Online: players connected (server connections less the keeper's own and players' match and spectating connections). The hub alone is in no room, so it counts only there.")
    b.series("Rooms", [
        ("sfvr_keeper_rooms", "rooms"),
        ("sfvr_keeper_public_rooms", "public"),
        ("increase(sfvr_keeper_rooms_asked_total[10m])", "made in 10 min"),
        ("increase(sfvr_keeper_rooms_closed_total[10m])", "closed in 10 min"),
    ], w=8)
    b.series("Tables by phase", [("sfvr_keeper_tables", "{{phase}}")], w=8, stack=True)
    b.series("Sets ended (per 10 min)", [("sum by (end) (increase(sfvr_keeper_sets_total[10m]))", "{{end}}")], w=8, stack=True,
             description="result: back to the room; again: played on; forfeit: a player left; unfinished: called again.")
    b.series("Calls and tickets (per minute)", [
        ("rate(sfvr_keeper_calls_total[1m]) * 60", "calls"),
        ("rate(sfvr_keeper_calls_lapsed_total[1m]) * 60", "calls lapsed"),
        ("rate(sfvr_keeper_tickets_total[1m]) * 60", "tickets"),
    ], w=8)
    b.series("Relays", [
        ("sfvr_keeper_relays", "relays running"),
        ("sfvr_keeper_relay_viewers", "viewers"),
    ], w=8)
    b.series("Trouble", [
        ('increase(sfvr_keeper_kicks_total{why="flood"}[10m])', "chat flood kicks (10 min)"),
        ('increase(sfvr_keeper_kicks_total{why="owner"}[10m])', "owner removals (10 min)"),
        ("increase(sfvr_keeper_server_lost_total[10m])", "lost the server (10 min)"),
    ], w=12)
    b.series("Containers: server and keeper", [
        ('rate(sfvr_container_cpu_seconds_total{service=~"server|keeper"}[1m])', "{{service}} CPU"),
    ], unit="percentunit", w=12)

    b.row("Rooms API (the publisher)")
    b.stat("Last push", "time() - sfvr_publisher_last_success_seconds", unit="s", w=4, thresholds=RED_AT(60, 300), decimals=0,
           description="Seconds since the room list last reached the rooms API.")
    b.stat("Failures (1 h)", "increase(sfvr_publisher_failures_total[1h])", w=4, thresholds=RED_AT(1, 10), decimals=0)
    b.series("Pushes", [
        ("increase(sfvr_publisher_pushes_total[5m])", "pushes (5 min)"),
        ("increase(sfvr_publisher_failures_total[5m])", "failures (5 min)"),
    ], w=16, h=4)
    b.write("sfvr-rooms-service.json")


def vps():
    b = Board("sfvr-rooms-vps", "SFVR rooms VPS", "The VPS's health (node_exporter) and each container (container-stats).")
    b.row("Now")
    b.stat("Up", "time() - node_boot_time_seconds", unit="s", w=4, decimals=0)
    b.stat("CPU busy", '1 - avg(rate(node_cpu_seconds_total{mode="idle"}[1m]))', unit="percentunit", w=4, thresholds=RED_AT(0.7, 0.9))
    b.stat("CPUs", 'count(count by (cpu) (node_cpu_seconds_total{mode="idle"}))', w=4)
    b.stat("Memory used", "1 - node_memory_MemAvailable_bytes / node_memory_MemTotal_bytes", unit="percentunit", w=4, thresholds=RED_AT(0.8, 0.9))
    b.stat("Disk used", '1 - node_filesystem_avail_bytes{mountpoint="/"} / node_filesystem_size_bytes{mountpoint="/"}', unit="percentunit", w=4, thresholds=RED_AT(0.8, 0.9))
    b.stat("TCP connections", "node_netstat_Tcp_CurrEstab", w=4)
    b.stat("Containers down", 'count(sfvr_container_up == 0) or vector(0)', w=4, thresholds=RED_AT(1, 1),
           description="Containers of the stack that are not running.")
    b.stat("Restarts (24 h)", 'sum(increase(sfvr_container_restarts[24h])) or vector(0)', w=4, thresholds=RED_AT(1, 5), decimals=0)

    b.row("Host")
    b.series("CPU", [('sum by (mode) (rate(node_cpu_seconds_total{mode!="idle"}[1m])) / scalar(count(count by (cpu) (node_cpu_seconds_total{mode="idle"})))', "{{mode}}")],
             unit="percentunit", w=12, stack=True, description="Steal: the VPS host gives the CPU to others (a busy neighbour).")
    b.series("Load", [("node_load1", "1 min"), ("node_load5", "5 min"), ('count(count by (cpu) (node_cpu_seconds_total{mode="idle"}))', "CPUs")], w=12)
    b.series("Memory", [
        ("node_memory_MemTotal_bytes - node_memory_MemAvailable_bytes", "used"),
        ("node_memory_MemAvailable_bytes", "available"),
        ("node_memory_SwapTotal_bytes - node_memory_SwapFree_bytes", "swap used"),
    ], unit="bytes", w=12)
    b.series("Disk", [
        ('node_filesystem_size_bytes{mountpoint="/"} - node_filesystem_avail_bytes{mountpoint="/"}', "used"),
        ('node_filesystem_avail_bytes{mountpoint="/"}', "free"),
        ("sum(rate(node_disk_written_bytes_total[1m]))", "written/s"),
        ("sum(rate(node_disk_read_bytes_total[1m]))", "read/s"),
    ], unit="bytes", w=12)
    NIC = 'device!~"lo|docker.*|veth.*|br-.*"'
    b.series("Network", [
        (f"sum(rate(node_network_receive_bytes_total{{{NIC}}}[1m])) * 8", "in"),
        (f"sum(rate(node_network_transmit_bytes_total{{{NIC}}}[1m])) * 8", "out"),
    ], unit="bps", w=12)
    b.series("Packets and UDP", [
        (f"sum(rate(node_network_receive_packets_total{{{NIC}}}[1m]))", "packets in/s"),
        (f"sum(rate(node_network_transmit_packets_total{{{NIC}}}[1m]))", "packets out/s"),
        ("rate(node_netstat_Udp_InErrors[1m])", "UDP errors/s"),
        ("rate(node_netstat_Udp_RcvbufErrors[1m])", "UDP buffer full/s"),
    ], w=12, description="UDP: the keeper's links. Buffer-full errors: packets dropped because the keeper did not read them in time.")

    b.row("Containers")
    b.series("CPU by container", [('sum by (service) (rate(sfvr_container_cpu_seconds_total[1m]))', "{{service}}")],
             unit="percentunit", w=12, stack=True, description="1 = one CPU core.")
    b.series("Memory by container", [('sum by (service) (sfvr_container_memory_bytes)', "{{service}}")],
             unit="bytes", w=12, stack=True)
    b.series("TCP sockets (the rooms server's players)", [
        ("node_sockstat_TCP_inuse", "in use"), ("node_netstat_Tcp_CurrEstab", "established"),
        ("rate(node_netstat_Tcp_PassiveOpens[1m]) * 60", "new per minute"),
    ], w=12)
    b.series("Monitoring's own storage", [
        ("prometheus_tsdb_storage_blocks_bytes + prometheus_tsdb_head_chunks_storage_size_bytes", "prometheus"),
    ], unit="bytes", w=12)
    b.write("sfvr-rooms-vps.json")


def logs():
    b = Board("sfvr-rooms-logs", "SFVR rooms logs", "Every container's log and the host's journal (Loki, 14 days).",
              refresh="", time_from="now-1h")
    b.textbox("filter", "Contains (regex)", "")
    b.log_series("Log lines per minute", [
        ('sum by (service) (count_over_time({job="containers"} |~ "$filter" [1m]))', "{{service}}"),
    ], h=6)
    b.logs("Keeper", '{job="containers", service="keeper"} |~ "$filter"', h=12)
    b.logs("Rooms server", '{job="containers", service="server"} |~ "$filter"', w=12, h=10)
    b.logs("TURN credentials (ice) and publisher", '{job="containers", service=~"ice|publisher"} |~ "$filter"', w=12, h=10)
    b.logs("Errors and warnings, every container",
           '{job="containers"} |~ "(?i)error|fail|cannot|lost|refus|denied|warn" |~ "$filter"', h=10)
    b.logs("SSH logins", '{job="host", identifier="sshd"} |~ "Accepted|Failed|Invalid|Disconnected" |~ "$filter"', w=12, h=10)
    b.logs("Firewall blocks (ufw)", '{job="host"} |= "UFW BLOCK" |~ "$filter"', w=12, h=10)
    b.logs("Host: docker, kernel, system", '{job="host", identifier!="sshd"} != "UFW BLOCK" |~ "$filter"', h=10)
    b.write("sfvr-rooms-logs.json")


if __name__ == "__main__":
    service()
    vps()
    logs()
