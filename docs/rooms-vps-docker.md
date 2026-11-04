# Custom rooms server on a fresh Ubuntu VPS, in Docker

How to set up the server the rooms builds (0.3.0-rooms.N) use, on its own
VPS until rooms ship to everyone. It runs three containers from
`docker/compose.yaml`:

| Container | What it does | Network |
| --- | --- | --- |
| `server` | The rooms server (Weyvelength), the same commit as today's public server | TCP 5555 (and 443, redirected) |
| `ice` | Fresh Cloudflare TURN credentials for it every 6 hours | outbound HTTPS only |
| `keeper` | sfvr-keeper: custom rooms' tables, calls, rotation and moderation; a client of the server | UDP 47200–47299 (its peer-to-peer links) |

Reports stay on the main VPS; nothing here receives them.

Each container runs as an unprivileged user (uid 10001) with a read-only
filesystem, no Linux capabilities and no privilege escalation. What they
keep lives in two Docker volumes: `ice` (the TURN credentials file) and
`keeper` (the keeper's state: its lobby channel and rooms' tables, read back
after a restart). The keeper's signing key is a file secret.

They use the host's network (`network_mode: host`): ports that Docker
publishes go around ufw, which holds the per-address connection limit and
the 443 redirect, and the keeper's peer-to-peer links need the VPS's own
address. The firewall below is therefore what protects the ports.

## 1. The VPS

- Ubuntu 24.04 LTS, x86-64. 1 vCPU and 1–2 GB of RAM are plenty (the keeper
  used 1.9 s of CPU and 11 MB for 22 players at 10 tables).
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

On your PC, after any keeper change, copy the keeper's sources from the mod
repository into this one (it writes `deploy/keeper/`):

```powershell
D:\SteamLibrary\steamapps\common\StreetFighterV\sfv-rollback\scripts\keeper-bundle.ps1
```

Then pack this repository (the `rooms` branch) and copy it over:

```powershell
cd D:\SteamLibrary\steamapps\common\StreetFighterV
tar -czf sfvr-rooms.tgz --exclude=.git -C sfvr-server .
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
sudo touch keeper-words.txt
```

- `.env`: the server's TCP port (5555) and the keeper's UDP range. If you
  change either, change the firewall rules to match.
- `cloudflare.env`: the Cloudflare Realtime TURN key id and API token
  (Cloudflare dashboard, Realtime, TURN). Without them players behind strict
  networks cannot connect (STUN only). Keep the file root-only: the token
  can mint relay credentials billed to the account.
- `keeper-words.txt`: the public rooms' name filter, one word a line
  (empty: no filter).

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
key>` in `sfv_rollback.ini`.

## Updating

| What | How | Effect on players |
| --- | --- | --- |
| Ubuntu | unattended-upgrades (above); `sudo apt full-upgrade` now and then | a reboot ends rooms and matches |
| A new keeper | `keeper-bundle.ps1`, copy `deploy/keeper/` over (step 5), then `sudo docker compose up -d --build keeper` | rooms stay; their tables pause a few seconds and carry on |
| Security fixes in the images | `sudo docker compose build --pull && sudo docker compose up -d` (monthly, or after an Ubuntu advisory) | the server restarts: rooms and matches end |
| Cloudflare key | edit `cloudflare.env`, then `sudo docker compose up -d ice` | none |
| Weyvelength commit | change it in `docker/server.Dockerfile`, `deploy/install.sh`, the mod's `CMakeLists.txt` and `src/keeper/CMakeLists.txt` together | needs a mod release |

Old images pile up after rebuilds: `sudo docker image prune -f`.

## Day to day

```bash
cd /opt/sfvr-rooms/docker
sudo docker compose ps                    # what runs
sudo docker compose logs -f keeper        # follow the keeper (Ctrl+C stops following)
sudo docker compose restart keeper        # restart one container
sudo docker compose down                  # stop everything (volumes stay)
```

Logs rotate on their own (10 MB, 5 files per container).

## When something is wrong

- **Custom rooms show as offline in the game**: the build does not trust this
  keeper (its `kKeeperPublicKey`, or the ini's `keeper_key`, is another
  key), or the keeper is not connected (`docker compose logs keeper`).
- **Players cannot connect at all**: the firewall (step 3, and the
  provider's), or something else on 5555 (`sudo ss -ltnp 'sport = :5555'`).
- **Matches fail for some players only**: no TURN (`docker compose logs ice`;
  `cloudflare.env`).
- **The keeper's links fail**: UDP 47200–47299 closed somewhere.

## Tested

2026-11-04, on a Windows PC with Docker Desktop (the stack with
`compose.local.yaml`, which publishes the ports instead of host networking):
both images built (the keeper's first Linux build), the keeper made its key
and connected, and two game instances played a table set through it (tickets,
result, the next call; 0 desyncs); the keeper read its state back after a
restart. Not yet tried on a real VPS: host networking, the ufw scripts with
Docker beside them, and minting with a real Cloudflare key.
