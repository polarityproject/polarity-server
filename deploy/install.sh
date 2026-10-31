#!/bin/sh
# Installs (or updates) sfvr-server, SFVR's rooms server
# (Weyvelength), on a Debian or Ubuntu VPS, with Cloudflare TURN when
# /etc/sfvr-server/cloudflare.env has a key. Everything it adds is named
# sfvr-server (user, services, /opt, /etc and /var/lib folders), so other
# servers can share the machine; uninstall.sh removes it again.
# Run as root from this folder: sudo sh install.sh
# Guide: docs/server-vps.md in the sfvr-server repository.
set -eu

NAME=sfvr-server
# Keep in step with the mod repository's CMakeLists.txt (FetchContent
# weyvelength): the server and the clients speak the protocol of the same
# commit. The mod's patch (third_party/weyvelength.patch) only changes the
# client.
COMMIT=d4e25c7da7aab18ff82d60d275ea95f794bb2ec9
REPO=https://github.com/HeatXD/Weyvelength.git
HOME_DIR=/opt/$NAME
SRC=$HOME_DIR/src
BIN=$HOME_DIR/bin
ETC=/etc/$NAME
STATE=/var/lib/$NAME
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo sh $0" >&2
	exit 1
fi

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake git curl python3 >/dev/null

echo "== user $NAME"
id "$NAME" >/dev/null 2>&1 || useradd --system --home-dir "$STATE" --shell /usr/sbin/nologin "$NAME"

echo "== build ($COMMIT)"
if [ ! -d "$SRC/.git" ]; then
	mkdir -p "$HOME_DIR"
	git clone --quiet "$REPO" "$SRC"
fi
git -C "$SRC" fetch --quiet origin
git -C "$SRC" checkout --quiet "$COMMIT"
git -C "$SRC" submodule update --init --recursive --quiet
cmake -S "$SRC" -B "$SRC/build" -DCMAKE_BUILD_TYPE=Release -DWEYVELENGTH_BUILD_TESTS=OFF >/dev/null
cmake --build "$SRC/build" --target Server -j "$(nproc)"

echo "== install into $HOME_DIR"
install -d -m 0755 "$BIN"
install -m 0755 "$SRC/build/Server/Server" "$BIN/weyvelength-server"
install -m 0755 "$SRC/docs/examples/mint-cloudflare-credentials.sh" "$BIN/mint-cloudflare-credentials.sh"
install -m 0755 "$HERE/sfvr-reports.py" "$BIN/sfvr-reports.py"
install -d -m 0755 "$ETC"
# The port (5555 unless changed here), and the Cloudflare key: root only
# (systemd reads both for the services).
[ -f "$ETC/server.env" ] || install -m 0644 "$HERE/server.env.example" "$ETC/server.env"
[ -f "$ETC/cloudflare.env" ] || install -m 0600 "$HERE/cloudflare.env.example" "$ETC/cloudflare.env"
install -m 0644 "$HERE/$NAME.service" "$HERE/$NAME-ice.service" "$HERE/$NAME-ice.timer" "$HERE/sfvr-reports.service" /etc/systemd/system/
install -d -o "$NAME" -g "$NAME" -m 0750 "$STATE"
# Desync reports (sfvr-reports.service); pulled with sudo (docs/server-vps.md).
install -d -o "$NAME" -g "$NAME" -m 0755 "$STATE/reports"
# Until TURN is set up: STUN alone, so players behind strict NATs (some
# mobile and corporate networks) cannot connect. No "expires" line: the
# server rereads the file every 5 minutes, so minted credentials replace
# this by themselves.
if [ ! -f "$STATE/ice.conf" ]; then
	printf '# STUN only until Cloudflare TURN is set up (docs/server-vps.md)\nstun stun.cloudflare.com 3478\n' > "$STATE/ice.conf"
	chown "$NAME:$NAME" "$STATE/ice.conf"
	chmod 0640 "$STATE/ice.conf"
fi

echo "== services"
systemctl daemon-reload
if grep -q '^CF_TURN_KEY_ID=..*' "$ETC/cloudflare.env" && grep -q '^CF_TURN_API_TOKEN=..*' "$ETC/cloudflare.env"; then
	if systemctl start "$NAME-ice.service"; then
		echo "TURN credentials minted from Cloudflare"
	else
		echo "minting TURN credentials failed: journalctl -u $NAME-ice (the server starts with STUN only)" >&2
	fi
	systemctl enable --now "$NAME-ice.timer"
else
	echo "no Cloudflare key in $ETC/cloudflare.env: STUN only (no TURN relay)"
fi
systemctl enable "$NAME.service" sfvr-reports.service
systemctl restart "$NAME.service" sfvr-reports.service
sleep 1
systemctl --no-pager --lines=5 status "$NAME.service" sfvr-reports.service || true
PORT=$(sed -n 's/^SFVR_PORT=\([0-9]*\).*/\1/p' "$ETC/server.env")
REPORTS=$(sed -n 's/^SFVR_REPORTS_PORT=\([0-9]*\).*/\1/p' "$ETC/server.env")
echo "done: $NAME listens on TCP ${PORT:-5555}, desync reports on TCP ${REPORTS:-5580}; open both in the firewall (docs/server-vps.md)"
