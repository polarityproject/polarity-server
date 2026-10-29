# Changelog

Releases of sfvr-server, the rooms server for SFVR. A server release goes
with the mod builds that speak its Weyvelength protocol: the pinned commit
must match the mod's (see the README). Newest first; the format follows
[Keep a Changelog](https://keepachangelog.com).

## [Unreleased]

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

