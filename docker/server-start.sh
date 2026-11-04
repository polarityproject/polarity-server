#!/bin/sh
# The rooms server's start in its container: STUN only until the ice
# container has minted TURN credentials (the server rereads the file every
# 5 minutes, so they replace this by themselves), then the server.
set -eu
ICE=/data/ice.conf
if [ ! -f "$ICE" ]; then
	printf '# STUN only until Cloudflare TURN is set up (docs/rooms-vps-docker.md)\nstun stun.cloudflare.com 3478\n' > "$ICE"
	chmod 0640 "$ICE"
fi
# 6-character room codes: the mod's match codes.
exec /usr/local/bin/weyvelength-server --port "${SFVR_PORT:-5555}" --room-code-length 6 --ice-file "$ICE"
