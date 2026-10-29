# sfvr-server

The rooms server for the SFV rollback mod (the `sfv-rollback` repository):
[Weyvelength](https://github.com/HeatXD/Weyvelength), built from a pinned
commit and run on a Linux VPS with systemd, with Cloudflare TURN for pairs
of players that can't link directly. Matches run peer to peer; the server
only holds rooms and hands out STUN/TURN settings.

The public instance is `203.0.113.20:5555`, the mod's built-in default.

## Layout

| Path | What it is |
| --- | --- |
| `deploy/install.sh` | builds the server at the pinned commit and installs it and its services; run again to update |
| `deploy/uninstall.sh` | removes everything the installer added |
| `deploy/sfvr-server.service` | the rooms server |
| `deploy/sfvr-server-ice.service`, `.timer` | fetch TURN credentials from Cloudflare on a timer |
| `deploy/server.env.example`, `cloudflare.env.example` | the port, and the Cloudflare TURN key |
| `docs/server-vps.md` | the full guide: VPS, DNS, firewall, TURN, updating, troubleshooting |

## Install or update

```sh
scp -r deploy you@your-vps:~/sfvr-server
ssh you@your-vps 'cd ~/sfvr-server && sudo sh install.sh'
```

## Keeping in step with the mod

The server and the mod's clients must speak the same Weyvelength protocol.
`COMMIT` in `deploy/install.sh` must match the `GIT_TAG` of `weyvelength` in
the mod's `CMakeLists.txt`. When one moves, move the other, and update the
server when the matching mod build ships.

The mod builds its own Windows copy of the server from the same commit, for
local tests (`scripts/weyve-server.ps1` in the mod repository).
