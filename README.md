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
| `deploy/` | what the images build from: the keeper's sources (bundled from the client by its `scripts/keeper-bundle.ps1`), the Weyvelength patches, the reports service (`sfvr-reports.py`, `collect-reports.py`), and the host's firewall scripts (`connection-limit.sh`, `port-443.sh`) |
| `cloudflare/rooms-api/` | the rooms API Worker and its tests |
| `docs/install.md` | the full guide: a fresh VPS, the stack, monitoring, the rooms API, reports, updating and day-to-day running |
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

## Formatting

ruff 0.17.6 formats and checks the Python, set up in `ruff.toml`.
`ruff format .` formats it, `ruff format --check .` lists the unformatted
files, and `ruff check .` reports lint findings. The keeper's C++ follows the client's
formatting.

## Changes

See [CHANGELOG.md](CHANGELOG.md).

## Third-party material

The Weyvelength patches, the word filter list, the game's own text and the
libraries in the images keep their own licenses. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
