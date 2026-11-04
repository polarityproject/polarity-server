#!/bin/sh
# Installs (or updates) sfvr-keeper beside sfvr-server (install.sh first):
# builds it from the sources in this folder (the mod repository's, copied
# by its scripts/keeper-bundle.ps1), makes its signing key the first time
# (and prints the public half for the mod), opens its UDP range in ufw and
# starts sfvr-keeper.service.
# Run as root from this folder: sudo sh install-keeper.sh
set -eu

NAME=sfvr-server
HOME_DIR=/opt/$NAME
BIN=$HOME_DIR/bin
ETC=/etc/$NAME
STATE=/var/lib/$NAME
PORTS=47200:47299
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo sh $0" >&2
	exit 1
fi
if ! id "$NAME" >/dev/null 2>&1; then
	echo "install sfvr-server first (sudo sh ../install.sh)" >&2
	exit 1
fi

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq build-essential cmake git libssl-dev >/dev/null

echo "== build"
[ -f "$HERE/SOURCE.txt" ] && cat "$HERE/SOURCE.txt"
cmake -S "$HERE/src/keeper" -B "$HOME_DIR/keeper-build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$HOME_DIR/keeper-build" -j "$(nproc)"

echo "== install"
install -d -m 0755 "$BIN"
install -m 0755 "$HOME_DIR/keeper-build/sfvr-keeper" "$BIN/sfvr-keeper"
install -d -m 0755 "$ETC"
if [ ! -f "$ETC/keeper.key" ]; then
	"$BIN/sfvr-keeper" --new-key "$ETC/keeper.key"
	echo "^ put this public key in the mod (rooms_client.cpp kKeeperPublicKey) for builds that use this server"
fi
chown root:"$NAME" "$ETC/keeper.key"
chmod 0640 "$ETC/keeper.key"
# The name filter's words, one a line (empty: no filter).
[ -f "$ETC/keeper-words.txt" ] || install -m 0644 /dev/null "$ETC/keeper-words.txt"
install -d -o "$NAME" -g "$NAME" -m 0750 "$STATE/keeper"
install -m 0644 "$HERE/sfvr-keeper.service" /etc/systemd/system/

if command -v ufw >/dev/null 2>&1; then
	echo "== ufw: UDP $PORTS for the keeper's links"
	ufw allow "$PORTS/udp" comment sfvr-keeper >/dev/null
fi

echo "== service"
systemctl daemon-reload
systemctl enable --quiet sfvr-keeper.service
systemctl restart sfvr-keeper.service
sleep 2
systemctl --no-pager --lines=5 status sfvr-keeper.service || true
echo "public key: $BIN/sfvr-keeper prints it when made; it is also the last 128 hex digits of $ETC/keeper.key"
