# Running sfvr-server on a Linux VPS

sfvr-server is the mod's rooms server: Weyvelength, installed under its own
name so it can share the VPS with other, unrelated servers. Everything it
adds is called `sfvr-server`:

| What | Where |
| --- | --- |
| system user and group | `sfvr-server` |
| services | `sfvr-server` (the rooms server), `sfvr-server-ice` and its timer (TURN credentials) |
| source and binaries | `/opt/sfvr-server` (`src/`, `bin/`) |
| settings | `/etc/sfvr-server` (`server.env`: the port; `cloudflare.env`: the TURN key) |
| state | `/var/lib/sfvr-server` (`ice.conf`, the STUN/TURN set handed to clients) |

`uninstall.sh` removes all of it again and nothing else.

The mod finds other players through a Weyvelength rooms server: hosting
makes a room, the match code is the room's code, and the server passes the
two games' connection details to each other. After that the match runs
**peer to peer** over UDP. When two players' networks can't reach each other
directly (strict NATs: some mobile, campus and corporate networks), the link
goes through **Cloudflare's TURN relay** instead. The mod says which one each
session got.

What runs where:

| | Where | Traffic |
| --- | --- | --- |
| Rooms server (sfvr-server) | your VPS, TCP 5555 (`server.env` can change it) | room state only, a few KB per session |
| STUN (finding your public address) | Cloudflare, `stun.cloudflare.com:3478` | a few packets per link |
| TURN relay (only for pairs that can't link directly) | Cloudflare, `turn.cloudflare.com`, UDP 3478 and 53 | the match itself: small input packets every frame |
| Matches | the two players' PCs | peer to peer, or through the relay |

The VPS never carries match traffic, so the smallest plan is plenty (1 vCPU,
512 MB to 1 GB RAM). No UDP ports need opening on it.

The files are in `deploy/` in this repository:

| File | What it is |
| --- | --- |
| `install.sh` | builds the server at the mod's pinned Weyvelength commit, installs it and the services; run again to update |
| `uninstall.sh` | removes the services, the user and the three folders |
| `server.env.example` | the port, installed as `/etc/sfvr-server/server.env` |
| `sfvr-server.service` | the rooms server, as user `sfvr-server`, restarted if it stops |
| `sfvr-server-ice.service` | fetches TURN credentials from Cloudflare into the server's ice file |
| `sfvr-server-ice.timer` | runs that every 6 hours (credentials last 24) |
| `cloudflare.env.example` | where the Cloudflare key goes |

Tested on Ubuntu 26.04 with systemd (in WSL): the build, the services, the
minting job against a stand-in for Cloudflare's API (good and bad token), a
real match and a spectator through a TURN relay (coturn standing in for
Cloudflare), the switch between relayed and peer-to-peer links, a changed
port, and `uninstall.sh` leaving nothing behind. Not yet tried against
Cloudflare itself, which needs the account's key.

## 1. The VPS

Any provider with Ubuntu 24.04 or newer (or Debian 12). Pick a region near
most players: the server only matters when joining, but closer is snappier.

Point a DNS name at it, for example an A record `sfv.example.com` to the
VPS's address. The mod can also take a bare IP, but a name lets you move the
server later without everyone changing settings.

Log in, update, and set up the firewall (SSH first, or you lock yourself out):

```sh
sudo apt update && sudo apt upgrade -y
sudo ufw allow OpenSSH
sudo ufw limit 5555/tcp comment sfvr-server   # for now: 6 connections in 30 s per address (see "Connection limit")
sudo ufw allow 5580/tcp comment sfvr-reports  # desync and crash reports (see "Desync and crash reports")
sudo ufw enable
```

The server has no passwords or TLS, and a room code is the only thing
protecting a room. `ufw limit` keeps one address from flooding it with
connections. A player makes one connection per session, so that limit
never gets in their way.

## 2. A Cloudflare TURN key

1. Sign in to the Cloudflare dashboard.
2. Under **Realtime**, open the TURN section and create a TURN key. The
   wording in the dashboard may differ; Cloudflare's guide is at
   https://developers.cloudflare.com/realtime/turn/.
3. Copy the key's ID and its API token. The token may only be shown once.

TURN is metered on relayed traffic, and only pairs that can't connect
directly use it. A relayed match's traffic is not measured yet; inputs are
small packets sent every frame, so expect a few MB per match. Check Cloudflare's
current TURN pricing and free allowance before opening it to many players.

You can skip this step and add the key later. Without it the server hands
out STUN only, which is enough for most home connections. Pairs that need a
relay then can't connect at all; they get "connection lost" while joining.

## 3. Install

Copy `deploy/` to the VPS, for example from this repository on your PC:

```sh
scp -r deploy you@sfv.example.com:~/sfvr-server
```

Then on the VPS, put the key in and run the installer:

```sh
cd ~/sfvr-server
sudo install -d -m 0755 /etc/sfvr-server
sudo install -m 0600 cloudflare.env.example /etc/sfvr-server/cloudflare.env
sudo nano /etc/sfvr-server/cloudflare.env     # fill in CF_TURN_KEY_ID and CF_TURN_API_TOKEN
sudo sh install.sh
```

If another server on the machine already uses port 5555, put a different
one in `/etc/sfvr-server/server.env` (`SFVR_PORT=5600`, say) before
installing, or afterwards plus `sudo systemctl restart sfvr-server`. Open
that port in the firewall instead, and use it in the mod (`host:5600`).

### Connection limit

ufw's `limit` rule of step 1 refuses an address that opens 6 connections in
30 seconds, and every refused try counts again, so retrying keeps it out.
Players who share an address (two PCs behind one router, a LAN, a provider's
carrier-grade NAT) hit that after a few quick hosts and joins: the game says
it cannot reach the server for a minute. Replace it with a looser limit (20
new connections at once, then 30 a minute per address; tries beyond that
are dropped without counting):

```sh
sudo sh connection-limit.sh       # in deploy/; `sudo sh connection-limit.sh off` puts ufw's limit back
```

It adds a rule to `/etc/ufw/before.rules` (between `sfvr-server` comment
lines) and turns `ufw limit 5555/tcp` into `ufw allow 5555/tcp`. It covers
443 too when the redirect below is there. The mod opens one connection per
host or join (443 only when 5555 does not answer within a moment).

### Port 443 as well

Some players' networks (work, school, some ISPs and routers) let only common
ports through: their game gets no answer on 5555 at all, while the update
check over HTTPS works. The mod connects to the public server on 5555, and
on 443 too when 5555 does not answer within a moment, so the server should
answer on 443 as well:

```sh
sudo ss -ltnp 'sport = :443'      # nothing listed: 443 is free
sudo sh port-443.sh               # in deploy/; `sudo sh port-443.sh off` removes it
```

It adds a ufw NAT rule (at the top of `/etc/ufw/before.rules`, between
`sfvr-server` comment lines) that redirects TCP 443 to the server's port, so
both ports reach the same process and the same rooms, and the connection
limit covers 443 too. Nothing else may use 443 on the machine: the script
refuses if something listens there, or if `before.rules` already has a `*nat`
section (it prints the line to add by hand). Check from a Windows PC with
`Test-NetConnection <address> -Port 443` in PowerShell.

The installer:

1. Installs the build tools.
2. Creates a system user `sfvr-server`.
3. Clones Weyvelength into `/opt/sfvr-server/src` at the commit the mod is built with, and builds the server (a minute or two).
4. Installs it as `/opt/sfvr-server/bin/weyvelength-server`.
5. Fetches the first TURN credentials and starts the timer.
6. Starts the rooms server.

At the end it prints the service status. You want `active (running)` and lines like:

```
Listening on port 5555
Ice: stun stun.cloudflare.com:3478
Ice: turn turn.cloudflare.com:3478 as <username>
Ice: turn turn.cloudflare.com:53 as <username>
```

Without a key it says "STUN only" and shows just the `stun` line.

## 4. Point the mod at it

**For testing, per PC:** open the panel (F1, or Back + Start), go to the
**Debug** tab, and under **Server** type `sfv.example.com:5555`, then press
**Use**. That server is then used for the next host, join or watch, and is
saved with the panel's settings. Servers you used before show as buttons
under the field, and **This PC** switches back to a local test server.
Settings, then Server, shows which one is in use.

**For everyone:** the mod's built-in default is the public server,
`203.0.113.20:5555` (`kDefaultServer` in the mod repository's
`src/mod/netplay_driver.h`). It is used whenever `[weyve] server` is empty
or missing, and the shipped `sfv_rollback.ini` (the mod's
`scripts/package.ps1`) writes the same address. If the
server moves, change both (or point a DNS name at it and use the name):

```ini
[weyve]
server=203.0.113.20:5555
```

Both players must use the same server: a code only exists on the server that
made it.

## 5. Check it works

From two PCs on different networks (or one PC and one phone hotspot):

1. One hosts, the other joins with the code.
2. In the character select panel, under each player's name, the ping is
   shown with **Peer to peer**, or **Relayed via Cloudflare** in yellow when
   the link went through TURN.
3. In a match, the other player's bar at the top of the screen shows **P2P**
   or **RELAY** before the ping.

To test the relay on purpose: in the Debug tab, tick **Relay only** on both
PCs *before* hosting and joining. Links then use only relay candidates, so
the session either connects through Cloudflare's TURN or doesn't connect at
all. The mod's log says `netplay: the link to the other side is relayed
through turn.cloudflare.com`.

## Desync and crash reports

`install.sh` also installs `sfvr-reports` (`deploy/sfvr-reports.py`, Python's
standard library only, service `sfvr-reports`). SFVR games send it reports
unless the player turned it off (Settings, Logs):

- **Desyncs**, after a match that had one: the desync's recording, the
  match's recording and its log. Filed by match id, so both players' halves
  of a desync land in one folder.
- **Crashes**, at the next start after a run that ended badly: the end of
  the run's log with what ended it, and a small minidump (threads, stacks,
  modules; no heap) when there was a fault. Filed by day.

```text
/var/lib/sfvr-server/reports/<match id>/<file names as the game saved them>
/var/lib/sfvr-server/crashes/<date>/<date>_<time>_crash_<id>.log (and .dmp)
<each folder>/received.txt   # when each file came, from which SFVR version
```

- **Port**: TCP 5580 (`SFVR_REPORTS_PORT` in `/etc/sfvr-server/server.env`),
  plain HTTP. Open it in the firewall (`sudo ufw allow 5580/tcp`). The mod's
  built-in address is `http://203.0.113.20:5580`; `[report] url` in
  `sfv_rollback.ini` sends elsewhere.
- **What it takes**: only file names the mod gives its recordings, match
  logs and crash reports, only files that start as the mod writes them, at
  most 16 MB each, 120 files an hour from one address. Anything else gets a
  4xx answer and is not stored. A file it already has is not stored again.
- **Disk**: past `SFVR_REPORTS_MAX_MB` (4096) the matches that have gone
  longest without an upload are removed; crashes past `SFVR_CRASHES_MAX_MB`
  (1024), the oldest days.
- **No addresses kept**: the journal and `received.txt` say what arrived, not
  from where.

To pull them to your PC, from the mod repository (an SSH key login to the
VPS; `-Sudo` if the login is not root):

```powershell
scripts\pull-reports.ps1 -Login root@203.0.113.20
```

Only files you don't have yet are copied, into
`StreetFighterV\Binaries\Win64\reports\desyncs\<match id>` and
`...\reports\crashes\<date>`. A match with both players' desync reports is
compared with `sfvr peers` (`peers.txt` in its folder); each new crash
report's fault (or "no clean exit") is listed. A minidump opens in Visual
Studio or WinDbg; the mod's PDB names its frames (the game has no symbols:
its frames stay StreetFighterV.exe+offset).

## Day to day

```sh
journalctl -u sfvr-server -f                 # the server's log, live
journalctl -u sfvr-server-ice                # the credential fetches
journalctl -u sfvr-reports -f                # desync and crash reports as they arrive
systemctl list-timers sfvr-server-ice.timer  # when the next fetch runs
sudo systemctl restart sfvr-server           # restart (drops rooms; matches in progress go on)
```

- **Updating**: when the mod moves to a newer Weyvelength commit, the commit
  in `install.sh` changes with it. Copy the new `deploy/` and run
  `sudo sh install.sh` again. Players should update the mod at the same time:
  server and clients speak the same protocol version.
- **Rooms are in memory**: a restart drops every room (a match in progress
  plays on, because the link doesn't need the server). Players in a lobby
  have to host again.
- **Credentials rotate by themselves**. If Cloudflare can't be reached, the
  fetch fails and writes nothing. The server keeps the credentials it has,
  which last 24 hours, and the next run 6 hours later tries again.
- **Removing it**: `sudo sh uninstall.sh` (from the same folder) stops and
  deletes the services, the `sfvr-server` user and its `/opt`, `/etc` and
  `/var/lib` folders. The build packages and your firewall rules stay; remove
  the rule with `sudo ufw status numbered` and `sudo ufw delete <n>`.
- **The API token** can mint relay credentials billed to your account. If
  it leaks, delete the TURN key in the dashboard and make a new one, then
  update `/etc/sfvr-server/cloudflare.env` and run
  `sudo systemctl start sfvr-server-ice`.

## If something is wrong

| Symptom | Look at |
| --- | --- |
| "cannot reach the server sfv.example.com:5555" | `systemctl status sfvr-server`; `sudo ufw status` (5555/tcp allowed); the name resolves to the VPS (`nslookup sfv.example.com`) |
| Joining hangs, then "connection lost", only for some pairs | no TURN: `journalctl -u sfvr-server \| grep Ice` should list two `turn` lines; check `journalctl -u sfvr-server-ice` for a 401 (wrong key or token) |
| Always "Relayed", even on good networks | normal for some networks (CGNAT on both ends); with **Relay only** ticked it is the point. It costs relay traffic, not playability |
| The minter fails with exit 22 | Cloudflare answered with an error: the key ID or token is wrong, or the key was deleted |

## How it fits the code

- The mod repository's `third_party/weyvelength.patch` (applied when the
  mod's build fetches Weyvelength) adds to the client:
  - `weyve_peer_link_path`: the link's selected ICE candidate pair; a
    `typ relay` candidate on either side means relayed.
  - `weyve_turn_host`: the relay the server handed out.
  - `weyve_set_relay_only`: drops a peer's non-relay candidates, for testing.
- The server is unpatched: the installer builds it from the same commit.
- The session driver reads the path of the link to the other side every
  half second (a spectator's: to the host). It logs changes and puts it in
  the panel's view (`NetView::linkPath`, `relayHost`).
- STUN and TURN are configured only on the server: every client is handed
  the set when it connects and on each rotation, and nothing about them
  lives in the mod's settings.
