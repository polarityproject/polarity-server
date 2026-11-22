# Custom rooms server on a fresh Ubuntu VPS, in Docker

How to set up Polarity's server in Docker: the server the rooms builds
(0.3.0-rooms.N) use, and from 0.3.0 the one public server (user,
2026-11-06: the rooms VPS takes over; only updates stay on the website). It
runs these containers from `docker/compose.yaml`:

| Container | What it does | Network |
| --- | --- | --- |
| `server` | The rooms server (Weyvelength), the same commit as today's public server | TCP 5555 (and 443, redirected) |
| `ice` | Fresh Cloudflare TURN credentials for it every 6 hours | outbound HTTPS only |
| `geoip` | DB-IP's free IP-to-country database for it, every month: players' countries for the rooms API | outbound HTTPS only |
| `keeper` | polarity-keeper: custom rooms' tables, calls, rotation and moderation; a client of the server | UDP 47200–47299 (its peer-to-peer links) |
| `reports` | sfvr-reports: the games' desync and crash reports, into the host's `/var/lib/sfvr-server/` ([Reports](#reports)) | TCP 5580 |

Until 0.3.0 the rooms builds still send their reports to the main VPS (their
`kDefaultReportUrl`); 0.3.0 sends them here, and [Moving to 0.3.0](#moving-to-030)
lists the rest of the switch.

Optional, beside them: the monitoring (`docker/compose.monitoring.yaml`,
[Monitoring](#monitoring)): dashboards, logs and alerts for the VPS and the
rooms service, all on the VPS and reachable only through SSH; and the
publisher that sends the public room list to the [rooms API](#rooms-api) on
Cloudflare, which sites and bots read.

Each container runs as an unprivileged user (uid 10001) with a read-only
filesystem, no Linux capabilities and no privilege escalation. What they
keep lives in Docker volumes: `ice` (the TURN credentials file), `keeper`
(the keeper's state: its desk and rooms' tables, read back after a restart),
`geoip` (the country database) and `stats` (the server's connection count
every 5 s, read by the keeper for the players online: players browsing
rooms are in no room). The keeper's signing key is a file secret.

They use the host's network (`network_mode: host`): ports that Docker
publishes go around ufw, which holds the per-address connection limit and
the 443 redirect, and the keeper's peer-to-peer links need the VPS's own
address. The firewall below is therefore what protects the ports.

## 1. The VPS

- Ubuntu 24.04 LTS, x86-64. 1 vCPU and 1–2 GB of RAM are plenty for the three
  containers (the keeper used 1.9 s of CPU and 11 MB for 22 players at 10
  tables). With the monitoring, plan 2 vCPU and 4 GB: it uses about 300 MB idle
  and its containers are capped at 1.9 GB together, and its disk grows to about
  5 GB (30 days of metrics, 14 of logs).
- Note its public IPv4 address. Below it is `203.0.113.10`; use yours.
- If the provider has its own firewall in front of the VPS, open the same
  ports there as in step 3.

## 2. First login and the basics

From your PC (the provider gives you root or an initial user):

```bash
ssh root@203.0.113.10
```

Make an admin user with your SSH key, then turn off root and password logins:

```bash
adduser polarityadmin
usermod -aG sudo polarityadmin
install -d -m 0700 -o polarityadmin -g polarityadmin /home/polarityadmin/.ssh
cp ~/.ssh/authorized_keys /home/polarityadmin/.ssh/ && chown polarityadmin:polarityadmin /home/polarityadmin/.ssh/authorized_keys
```

Check in a second window that `ssh polarityadmin@203.0.113.10` works and `sudo`
asks for the new password. Then:

```bash
sudo sed -i 's/^#\?PermitRootLogin .*/PermitRootLogin no/; s/^#\?PasswordAuthentication .*/PasswordAuthentication no/' /etc/ssh/sshd_config
sudo systemctl restart ssh
```

Security updates on their own, and a reboot at 05:00 when one needs it:

```bash
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y unattended-upgrades
sudo dpkg-reconfigure -f noninteractive unattended-upgrades
echo 'Unattended-Upgrade::Automatic-Reboot "true";
Unattended-Upgrade::Automatic-Reboot-Time "05:00";' | sudo tee /etc/apt/apt.conf.d/52sfvr-reboot
```

(A reboot ends the rooms and matches on the server; the containers come back
by themselves. Pick a quiet hour for your players, or set it to `"false"`
and reboot by hand.)

## 3. Firewall

```bash
sudo ufw allow OpenSSH
sudo ufw allow 5555/tcp comment sfvr-rooms
sudo ufw allow 47200:47299/udp comment sfvr-keeper
sudo ufw allow 5580/tcp comment sfvr-reports
sudo ufw enable
```

The two scripts from `deploy/` (copied in step 5) then replace ufw's own
connection limit with the looser one players need behind shared addresses,
and send TCP 443 to the server for networks that only let common ports out.
Run them after step 5:

```bash
cd /opt/sfvr-rooms/deploy
sudo sh connection-limit.sh
sudo sh port-443.sh
```

## 4. Docker

Docker Engine from Docker's own repository (Ubuntu's `docker.io` package
lags behind):

```bash
sudo apt install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | sudo tee /etc/apt/sources.list.d/docker.list
sudo apt update
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo systemctl enable --now docker
```

The commands below use `sudo docker`. Adding yourself to the `docker` group
instead saves typing, but that group is as good as root on the machine.

## 5. The files

On your PC, in the folder that holds the polarity-client and
polarity-server checkouts, copy the keeper's sources from the mod repository
into this one after any keeper change (it writes `deploy/keeper/`):

```powershell
polarity-client\scripts\keeper-bundle.ps1
```

Commit that, then pack the `main` branch and copy it over from the same
folder. `git archive` packs committed files only, so a test key or settings
file left in `docker/` from trying the stack on your PC stays there:

```powershell
git -C polarity-server archive --format=tar.gz -o sfvr-rooms.tgz main
scp sfvr-rooms.tgz polarityadmin@203.0.113.10:~
```

On the VPS:

```bash
sudo mkdir -p /opt/sfvr-rooms
sudo tar -xzf ~/sfvr-rooms.tgz -C /opt/sfvr-rooms
cd /opt/sfvr-rooms/docker
```

Now run the two firewall scripts from step 3.

## 6. Settings

```bash
sudo cp env.example .env
sudo cp ../deploy/cloudflare.env.example cloudflare.env
sudo chmod 0600 cloudflare.env
sudo cp keeper-words.default.txt keeper-words.txt
sudo groupadd --system --gid 10001 sfvr-reports
sudo usermod -aG sfvr-reports polarityadmin
sudo install -d -o 10001 -g 10001 -m 0750 /var/lib/sfvr-server/reports /var/lib/sfvr-server/crashes
```

- `.env`: the server's TCP port (5555), the keeper's UDP range and the
  reports' port (5580) and limits. If you change a port, change the
  firewall rules to match.
- `/var/lib/sfvr-server/reports` and `crashes`: where the reports container
  writes (host folders, so you read and copy them on the VPS itself), owned
  by its user (uid 10001). The `sfvr-reports` group has the same id, so the
  admin user in it can read and `scp` them without sudo (log in again after
  `usermod` for the group to apply). If `groupadd` says the id is taken,
  `getent group 10001` names the group to add the admin user to instead.
- `cloudflare.env`: the Cloudflare Realtime TURN key id and API token
  (Cloudflare dashboard, Realtime, TURN). Without them players behind strict
  networks cannot connect (STUN only). Keep the file root-only: the token
  can mint relay credentials billed to the account.
- `keeper-words.txt`: the public rooms' word filter (names and chat), one
  entry a line (empty: no filter). The default is the English list of
  LDNOOBW ("List of Dirty, Naughty, Obscene, and Otherwise Bad Words", CC BY
  4.0; its header has the credit and the format). Edit your copy; see
  [Word filter](#word-filter).

## 7. Build

```bash
sudo docker compose build
```

It builds the rooms server and the keeper from source (a few minutes the
first time).

## 8. The keeper's signing key

Made once, kept forever: the rooms builds trust only this key, so a new key
means a new mod build.

```bash
sudo install -d -m 0700 -o 10001 -g 10001 secrets
sudo docker run --rm -v "$PWD/secrets:/out" sfvr-rooms/keeper:latest --new-key /out/keeper.key
sudo chmod 0400 secrets/keeper.key
```

It prints the public key (128 hex digits). Keep a copy of
`secrets/keeper.key` somewhere safe off the VPS.

## 9. Start

```bash
sudo docker compose up -d
sudo docker compose ps
sudo docker compose logs --tail 20
```

Expect `Listening on port 5555` from the server, `keeper: connected to
127.0.0.1:5555` and `lobby channel …` from the keeper, and from `ice`
either `TURN credentials minted` or the STUN-only line.

From your PC, check the ports answer:

```powershell
Test-NetConnection 203.0.113.10 -Port 5555
Test-NetConnection 203.0.113.10 -Port 443
```

## 10. Point the rooms builds at it

In the mod repository's `rooms` branch:

- `src/mod/netplay_driver.h`: `kDefaultServer = "203.0.113.10:5555"`. The
  mod tries TCP 443 beside 5555 only for this default server.
- `src/mod/rooms_client.cpp`: `kKeeperPublicKey`, the 64 bytes of the public
  key. To turn the printed hex into the array:

  ```bash
  python3 -c "import sys; h = sys.argv[1]; print(', '.join('0x' + h[i:i + 2] for i in range(0, 128, 2)))" <public key>
  ```

Before that build, a rooms build can test the server with its ini: `server`
set to `203.0.113.10:5555` in the panel, and `[rooms] keeper_key=<public
key>` in `polarity.ini`.

## Monitoring

Seven more containers from `docker/compose.monitoring.yaml`, every one on
127.0.0.1 only (nothing new is reachable from outside, no firewall change):

| Container | What it does |
| --- | --- |
| `node-exporter` | The VPS's CPU, memory, disk and network, and the keeper's metrics file (`sfvr_keeper_*`, written every 5 s) |
| `container-stats` | CPU, memory, state and restarts per container, from the Docker API (read-only socket) |
| `prometheus` | Keeps the metrics 30 days (at most 4 GB); evaluates the alert rules in `monitoring/rules.yml` |
| `loki` | Keeps the logs 14 days |
| `alloy` | Sends every container's log and the host's journal (sshd, ufw, Docker, the kernel) to Loki |
| `grafana` | The dashboards: rooms service, VPS, logs |
| `publisher` | Sends the public room list to the rooms API every 10 s (idle until it has an address) |

The keeper writes `keeper.prom` and `status.json` into its volume; the
exporter and the publisher read them in the keeper's group, read-only.

### Set it up

In `/opt/sfvr-rooms/docker`, once:

```bash
sudo sh -c 'openssl rand -base64 24 > secrets/grafana_admin'
sudo sh -c 'openssl rand -hex 32 > secrets/rooms_push'
sudo chmod 0444 secrets/grafana_admin secrets/rooms_push
sudo cp monitoring.env.example monitoring.env
sudo docker compose -f compose.yaml -f compose.monitoring.yaml up -d --build
sudo docker compose -f compose.yaml -f compose.monitoring.yaml ps
```

The two secrets are readable by the containers' users (Grafana and the
publisher do not run as root); `secrets/` itself stays root's. Leave
`ROOMS_API_URL` empty in `monitoring.env` until the [rooms API](#rooms-api) is
deployed.

From then on, use both files in every compose command (the `Updating` and
`Day to day` commands too), or the monitoring containers are left out:

```bash
alias dc='sudo docker compose -f compose.yaml -f compose.monitoring.yaml'
```

(in `~/.bashrc` to keep it).

### Open the dashboards

Grafana listens on the VPS's 127.0.0.1:3000. From your PC, through SSH:

```powershell
ssh -L 3000:127.0.0.1:3000 polarityadmin@203.0.113.10
```

Leave that session open and browse to http://localhost:3000. User `admin`,
the password `sudo cat secrets/grafana_admin` prints. The Polarity folder has:

- **Polarity rooms service** (the home page): the keeper's loop (how long one step
  takes, how busy it is, how much of it is log writing), members, rooms,
  tables by phase, sets, calls and tickets, relays and viewers, the publisher.
- **Polarity rooms VPS**: CPU (steal is a busy neighbour on the VPS host), load,
  memory, disk, network, UDP drops, each container's CPU and memory.
- **Polarity rooms logs**: log lines per minute, the keeper, the server, ice and
  the publisher, errors from every container, SSH logins, firewall blocks,
  the rest of the host's journal. The `Contains` box filters every panel.

The dashboards are provisioned read-only from `monitoring/dashboards/`; to
change one, edit `monitoring/make-dashboards.py`, run it, and copy the files
over as in step 5.

### Alerts

`monitoring/rules.yml`: the keeper not writing its metrics, the desk (the
lobby channel) down, the server's stats file stale (players online would
count only room members), slow or busy steps (the longest, and 1 in 100 over 50 ms), connects
to the server backing up, log writing that blocks the loop, resends; CPU,
memory and disk; a stack container down or restarting; the publisher failing.
They show in Grafana (Alerting, Alert rules, under the Prometheus data
source) and in Prometheus. Nothing is sent anywhere yet: a Grafana contact
point (a Discord webhook, email) can be added when wanted.

### Word filter

The keeper reads `keeper-words.txt` (`--words`) at start. In public rooms:

- A name the list rejects shows as `Player 1234` (four random digits, the
  same for every player in the room, new in each room), in the room, the
  room list, previews and the rooms API.
- Each public room carries the list (room data `wl0`.. and its hash `wlv`),
  and the game stars the listed words in chat as it shows them.
- Players can turn the Word filter off in Polarity settings: they then see names
  and chat in rooms as they are. The room list, previews and the rooms API
  stay filtered. Private rooms are never filtered.

After editing the file: `sudo docker compose kill -s HUP keeper` reads it
again and rewrites every public room's copy (no restart, no room closed).
The file is mounted into the container on its own, so edit it in place
(`sudo nano`; an editor that saves a new file and renames it leaves the
keeper on the old one until `sudo docker compose restart keeper`).
For one word at once: `echo "word <word>" | sudo docker compose exec -T keeper
tee /data/state.cmd` (also `word <word>*` for a prefix).

Entries are folded (case, look-alike digits and symbols, everything else
dropped); in chat they match whole words, `word*` any word starting with it,
a phrase only its words in a row; in names an entry of 5 letters or more
matches anywhere, a shorter one only as a whole word of the name.

### The keeper's numbers that matter

- **Step time**: the keeper is one loop over every room. A request waits for
  the step it arrives in, so a long step is a slow answer for everyone. On
  2026-11-06 the VPS answered a new room after 57 s and a play-on request after
  10 s under bot load; the step time and the log-writing share say whether
  that is CPU, log writing or something else.
- **Connects**: the keeper opens a connection to the server for each new
  room and each set's spectator relay. They run on worker threads (since
  2026-11-06), so a slow connect delays only that room;
  `sfvr_keeper_connects_pending` and the connect seconds show them.
- **Resends**: answers players did not acknowledge within 250 ms.
- **UDP buffer full** (VPS dashboard): link packets dropped because the keeper
  did not read them in time.

## Rooms API

The public room list for sites, Discord bots and stream overlays, served by
a Cloudflare Worker (`cloudflare/rooms-api`). The VPS pushes the list to it
(the `publisher`), so readers never reach the VPS and need no address of it.
The full specification for tool builders (every field, errors, caching,
reference tables) is `docs/rooms-api.md`; this section is about running it.

```
GET https://api.polaritymod.link/v1/rooms
{"version":1,"updated":1793992354396,"age":4,"stale":false,
 "attribution":"IP Geolocation by DB-IP (https://db-ip.com), CC BY 4.0",
 "rooms":[{"code":"4SLJEV","title":"Karin user looking for M. Bison!","title_id":3,
           "title_characters":["KRN","VEG"],"owner":"Owner","members":3,
           "capacity":16,"tables":2,"open_seats":2,"rules":"r1 ft2 bo3 t99 ws3",
           "faces":["RYU","KEN"],
           "players":[{"name":"Owner","country":"DE","face":"RYU"},
                      {"name":"PolarityDev","country":"FR","face":"KEN"},
                      {"name":"Player 3","country":null,"face":null}]}]}

GET .../v1/summary
{"version":1,"updated":...,"age":4,"stale":false,
 "totals":{"online":12,"in_public_rooms":3,"public_rooms":1}}
```

- Only public rooms are listed (as the game lists them, without the game's
  limit of what fits its lobby data). Names are as the room shows them (the
  word filter's `Player 1234`, always, whatever a player's own setting).
- `title`: the room's title, the English of the Street Fighter V Battle Lounge
  comment its owner picked (`null`: none; show something like "Owner's room").
  `title_id` is the comment's number in the game's table (0, 3, 5-18 are
  offered) and `title_characters` the character codes Id 3 names (`KRN`,
  `VEG`). The game sends only the number and codes; the keeper checks them
  and writes the text, so a modified game cannot put its own words in a title.
- `players`: every member in join order, with their country and character.
  The country is the rooms server's own lookup of the player's address in
  DB-IP's free database (`geoip`), never anything the game claims: the
  server gives each member the member data `@cc` and refuses `@` keys from
  clients. A VPN shows its exit's country; `null` when unknown. The game
  shows the same flags beside names, and a player who turns "Country flag"
  off in Polarity's settings sends member data `nocc=1`: the server then gives
  them no `@cc` at all (here as `null`), in every room. The notice about it is
  on the Polarity site.
- **Attribution**: DB-IP's licence (CC BY 4.0) asks whoever shows the countries
  to credit them: the `attribution` line, or `IP Geolocation by DB-IP` with a
  link to https://db-ip.com, beside the flags on a page or in an embed.
- The totals: `online`, every player connected (on the hub, at the desk or
  in any room, private ones too: the server's connection count, less the
  keeper's own and players' second connections for a match or to watch one);
  `in_public_rooms`, the members of the listed rooms;
  `public_rooms`, the listed rooms. Nothing else about private rooms.
- `stale` is true when the list is over 60 s old: the VPS, the keeper or the
  publisher is down; a page should say so rather than show it as live.
- Any site may read it (CORS `*`); answers are cached 10 s at Cloudflare.
- Storage is one R2 object. The free tier covers it: about 1,500–8,600
  writes a day (a push when the list changes, at least every 60 s), reads
  mostly from the cache. KV's free tier (1,000 writes a day) would not.

### Deploy it

On your PC, with Node and a Cloudflare login (`npx wrangler login`), from
`polarity-server/cloudflare/rooms-api`. The Worker is served at
`api.polaritymod.link` (`routes` in `wrangler.toml`), so the
`polaritymod.link` zone must be in the same Cloudflare account first; deploy
makes the DNS record and the certificate.

```powershell
node test.mjs
npx wrangler r2 bucket create polarity-rooms
npx wrangler secret put PUSH_SECRET
npx wrangler deploy
```

`secret put` asks for the value: paste what `sudo cat secrets/rooms_push`
prints on the VPS. `deploy` prints both of the Worker's addresses. On the
VPS, put the custom one in `monitoring.env`:

```
ROOMS_API_URL=https://api.polaritymod.link
```

then `dc up -d publisher` (with the alias above) and, after 10 s,
`dc logs publisher` and the address's `/v1/summary` in a browser. The
workers.dev address answers the same, for readers that still use it.

To change the secret: a new `secrets/rooms_push`, `npx wrangler secret put
PUSH_SECRET` with it, `dc up -d --force-recreate publisher`.
## Reports

The `reports` container takes the games' desync and crash reports (the
mod sends them unless the player turned it off: Polarity settings, Send
reports) on TCP 5580 and files them in the host's folders:

- `/var/lib/sfvr-server/reports/<match id>/` (both players' halves of a
  desync side by side)
- `/var/lib/sfvr-server/crashes/<date>/`

Limits (`.env`): 16 MB a file, `REPORTS_PER_HOUR` files an hour from one
address, and past `REPORTS_MAX_MB` / `CRASHES_MAX_MB` the oldest folders
go. Fixed in `sfvr-reports.py` (its options): 256 MB a day from one
address, 16 uploads at once (2 from one address), 64 open connections,
and 300 s for a whole request. A stored file is never replaced: the same
name with a different size is refused. `dc logs reports` shows each one
received; the `ReportsUnhealthy` and `ContainerDown` alerts cover it.

To look at them: copy them off with `scp` as the admin user (it reads them
through the `sfvr-reports` group, step 6):

```bash
scp -r polarityadmin@203.0.113.10:/var/lib/sfvr-server/reports/<match id> .
scp -r polarityadmin@203.0.113.10:/var/lib/sfvr-server/crashes/<date> .
```

or pack them on the VPS (and clear what was packed, so this one needs sudo)
with the host's Python, as before:

```bash
sudo python3 /opt/sfvr-rooms/deploy/collect-reports.py --crashes --out ~
```

then `scp` the zip to your PC (polarity-client's `docs/reviewing-reports.md`
for what to do with it). Without Python on the host, the image has it:

```bash
dc run --rm -v ~:/out --entrypoint python3 reports /usr/local/bin/collect-reports.py \
  --reports-dir /reports --crashes-dir /crashes --crashes --out /out
```

## Updating

| What | How | Effect on players |
| --- | --- | --- |
| Ubuntu | unattended-upgrades (above); `sudo apt full-upgrade` now and then | a reboot ends rooms and matches |
| A new keeper | `keeper-bundle.ps1`, copy `deploy/keeper/` over (step 5), then `sudo docker compose up -d --build keeper` | rooms stay; their tables pause a few seconds and carry on |
| Security fixes in the images | `sudo docker compose build --pull && sudo docker compose up -d` (monthly, or after an Ubuntu advisory) | the server restarts: rooms and matches end |
| Cloudflare key | edit `cloudflare.env`, then `sudo docker compose up -d ice` | none |
| The country database | nothing: `geoip` fetches each month's on its own (`dc logs geoip`), and the server reopens it | none |
| Weyvelength commit | change it in `docker/server.Dockerfile`, the client's `CMakeLists.txt` and `src/keeper/CMakeLists.txt` together | needs a mod release |

Old images pile up after rebuilds: `sudo docker image prune -f`.

## Day to day

```bash
cd /opt/sfvr-rooms/docker
sudo docker compose ps                    # what runs
sudo docker compose logs -f keeper        # follow the keeper (Ctrl+C stops following)
sudo docker compose restart keeper        # restart one container
sudo docker compose down                  # stop everything (volumes stay)
```

Logs rotate on their own (10 MB, 5 files per container). With the monitoring,
the dashboards show the same logs for 14 days ([Monitoring](#monitoring)).

## When something is wrong

- **Custom rooms show as offline in the game**: the build does not trust this
  keeper (its `kKeeperPublicKey`, or the ini's `keeper_key`, is another
  key), or the keeper is not connected (`docker compose logs keeper`).
- **Players cannot connect at all**: the firewall (step 3, and the
  provider's), or something else on 5555 (`sudo ss -ltnp 'sport = :5555'`).
- **Matches fail for some players only**: no TURN (`docker compose logs ice`;
  `cloudflare.env`).
- **The keeper's links fail**: UDP 47200–47299 closed somewhere.
- **No countries on the rooms API**: `dc logs geoip` (the download) and
  `dc logs server | grep GeoIp` (it says when it opened the database).
- **Rooms take long to make, play on falls back to the room, calls lag**: the
  keeper's loop waits. Its dashboard says on what: a long **connect** was the
  connection limit catching the keeper's own connections on the loopback
  (fixed in `connection-limit.sh` on 2026-11-06; `sudo sh connection-limit.sh`
  again replaces an older rule); long steps without that are CPU (the VPS
  dashboard's steal and the keeper's CPU) or log writing.
