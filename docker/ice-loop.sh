#!/bin/sh
# Cloudflare TURN credentials for the rooms server, in its own container
# (the systemd install's sfvr-server-ice timer): a fresh batch every 6 hours
# (they last CF_TURN_TTL, 24 h by default), into the ice file the server
# rereads. A failed run writes nothing (the server keeps what it has) and is
# tried again in 10 minutes. Without a key: STUN only, and this waits.
set -u
export ICE_FILE=/data/ice.conf
if [ -z "${CF_TURN_KEY_ID:-}" ] || [ -z "${CF_TURN_API_TOKEN:-}" ]; then
	echo "no Cloudflare key in cloudflare.env: STUN only (no TURN relay); restart this container after adding one"
	exec sleep infinity
fi
while :; do
	if /usr/local/bin/mint-cloudflare-credentials.sh; then
		echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) TURN credentials minted"
		sleep 21600
	else
		echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) minting failed; trying again in 10 minutes" >&2
		sleep 600
	fi
done
