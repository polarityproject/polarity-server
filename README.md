# polarity-server

The server side of [Polarity](https://polaritymod.link), unofficial online
play for Street Fighter V (the client is
[polarity-client](https://github.com/polarityproject/polarity-client)).
Matches run peer to peer between the players' games. The server holds the
custom rooms, calls their tables, hands out STUN/TURN settings, and takes
the desync and crash reports players choose to send.

The public instance runs this stack on one VPS. Its rooms API is at
<https://api.polaritymod.link>.

## What runs

The Docker stack in `docker/` (`compose.yaml`):

| Service | What it does |
| --- | --- |
| `server` | [Weyvelength](https://github.com/HeatXD/Weyvelength), the rooms server, built at a pinned commit with the patches in `deploy/` and `docker/` |
| `ice` | fetches TURN credentials from Cloudflare on a timer, for players who can't link directly |
| `geoip` | keeps the country database the server reads for players' flags |
| `keeper` | polarity-keeper, a client of the server: calls tables, handles moderation, signs the room listing and keeps the word filter list |
| `reports` | polarity-reports: receives desync and crash reports |

`compose.monitoring.yaml` adds Prometheus, Loki, Alloy and Grafana with the
dashboards in `docker/monitoring/`, and the publisher that sends the room
list to the rooms API.

`cloudflare/rooms-api/` is the rooms API, a Cloudflare Worker that serves the
published room list to the website, the Discord bot and other tools.

## Layout

| Path | What it is |
| --- | --- |
| `docker/` | the compose files, Dockerfiles, settings examples (`env.example`, `monitoring.env.example`) and the monitoring setup |
| `deploy/` | what the images build from: the keeper's sources (bundled from the client by its `scripts/keeper-bundle.ps1`), the Weyvelength patches, the reports service; also the older systemd install (`install.sh`) |
| `cloudflare/rooms-api/` | the rooms API Worker and its tests |
| `docs/rooms-vps-docker.md` | the full guide: a fresh VPS, the stack, monitoring, the rooms API, reports, updating |
| `docs/rooms-api.md` | the rooms API's specification, for tool builders |

Secrets stay on the VPS and out of git: the keeper's signing key
(`docker/secrets/`), the `.env` files and the word filter list.

## Keeping in step with the client

The server and the clients must speak the same Weyvelength protocol.
`WEYVELENGTH_COMMIT` in `docker/server.Dockerfile` must match the `GIT_TAG`
of `weyvelength` in the client's `CMakeLists.txt`. When one moves, move the
other, and update the server when the matching client release ships.

The keeper's sources in `deploy/keeper/` are a copy of the client's
`src/keeper/` and the code it shares; rebundle them when those change.

## Changes

See [CHANGELOG.md](CHANGELOG.md).
