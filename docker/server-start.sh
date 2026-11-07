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
# 6-character room codes: the mod's match codes. Members' countries from the
# database the geoip container keeps in /geoip (reopened when it changes;
# none until it is there). GEOIP_PRIVATE: a country for LAN addresses (tests).
# Its connection count every 5 s for the keeper's players-online total
# (players browsing rooms are in no room): /stats, shared read-only with it.
set -- --port "${SFVR_PORT:-5555}" --room-code-length 6 --ice-file "$ICE" --geoip /geoip/country.mmdb --stats /stats/server.json
if [ -n "${GEOIP_PRIVATE:-}" ]; then set -- "$@" --geoip-private "$GEOIP_PRIVATE"; fi
exec /usr/local/bin/weyvelength-server "$@"
