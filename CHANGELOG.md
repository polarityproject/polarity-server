# Changelog

Releases of sfvr-server, the rooms server for SFVR. A server release goes
with the mod builds that speak its Weyvelength protocol: the pinned commit
must match the mod's (see the README). Newest first; the format follows
[Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

### Added

- `deploy/collect-reports.py` (installed in `/opt/sfvr-server/bin`): packs
  the desync reports received so far (`--crashes`: the crash reports too)
  into one zip laid out like the game's `reports` folder, then deletes them
  from the server. See `docs/server-vps.md`, "Collecting them in one zip".

### Fixed

- The rooms server could be crashed by anyone who reached its port: a
  malformed message of a few bytes claiming gigabytes made it allocate them
  until the kernel killed it (three times on the rooms VPS, 2026-11-07).
  `weyvelength-decode-limit.patch` (the same patch as the mod's) caps what
  decoding one message may allocate; the Docker image and `install.sh` apply
  it. The Docker stack also caps the server's memory at 512 MB and the
  keeper's at 256 MB, so a fault there restarts only that container.
- `sfvr-reports` counts its folders again before removing any for space, so
  reports deleted outside it no longer make it remove matches early.

## [0.1.2] - 2026-10-31

Desync and crash reports from SFVR 0.2.0 games, and a looser connection limit.

### Added

- `deploy/connection-limit.sh`: a looser limit on new connections to the
  rooms server than ufw's `limit` (20 at once, then 30 a minute per address,
  tries beyond it not counted), which refused players sharing an address
  after a few quick hosts and joins. See `docs/server-vps.md`, "Connection
  limit".

- `sfvr-reports` (`deploy/sfvr-reports.py`, service `sfvr-reports`, TCP 5580):
  takes the desync reports SFVR games send and files them by match under
  `/var/lib/sfvr-server/reports`, both players' halves together, and crash
  reports by day under `/var/lib/sfvr-server/crashes` (`/v1/crashes/`). Only
  the mod's own file names and formats, 16 MB a file, 120 files an hour per
  address, `SFVR_REPORTS_MAX_MB` (4096) and `SFVR_CRASHES_MAX_MB` (1024) on
  disk. `install.sh` installs it (and Python 3); open TCP 5580. See
  `docs/server-vps.md`, "Desync and crash reports".

## [0.1.1] - 2026-10-29

The rooms server answers on TCP 443 as well, for players whose network blocks 5555.

### Added

- `deploy/port-443.sh`: the rooms server on TCP 443 as well (a ufw redirect to
  its port), for players whose network blocks 5555; SFVR 0.1.3 tries 443
  alongside 5555 for the public server. See `docs/server-vps.md`, "Port 443
  as well".

## [0.1.0] - 2026-10-29

The first public server, for SFVR 0.1.0.

### Added

- A Linux VPS install (`deploy/install.sh`) of the Weyvelength rooms server
  at commit `d4e25c7da7aab18ff82d60d275ea95f794bb2ec9`, as the `sfvr-server`
  systemd service under its own user, with 6-character room codes; running
  it again updates it, and `deploy/uninstall.sh` removes everything it added.
- Cloudflare TURN for pairs of players that can't link directly: credentials
  fetched every 6 hours into the server's ice file
  (`sfvr-server-ice.service` and `.timer`); STUN only until a key is set.
- The port in `/etc/sfvr-server/server.env` (5555 by default).
- The guide, `docs/server-vps.md`: VPS, firewall, TURN, updating and
  troubleshooting.

### Deployed

- 2026-10-29: `203.0.113.20:5555`, the mod's built-in default.

