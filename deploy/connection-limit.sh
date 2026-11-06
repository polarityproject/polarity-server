#!/bin/sh
# A looser limit on new connections to the rooms server than ufw's own
# `limit` rule (6 in 30 s per address, fixed). That one refused players who
# share an address (two PCs behind one router, a LAN, carrier-grade NAT)
# after a few quick hosts and joins, and every refused try counted again, so
# retrying kept them out. This one lets an address open 20 at once and 30 a
# minute after that; tries beyond it are dropped without counting. It covers
# TCP 443 too when port-443.sh redirects it (the redirect comes first).
# Not on the loopback: the rule sits above ufw's own "allow all on
# loopback", and the keeper beside the server (Docker, host networking) opens
# a connection to 127.0.0.1 for every room and every table set's relay room;
# limited, its connects waited on TCP retries and held up its whole loop
# (2026-11-06: a new room took 57 s, a play-on answer 10 s under load). Run
# again, the script replaces a rule without that.
# IPv4 only: over IPv6 the port is allowed without a limit (the mod connects
# to the public server's IPv4 address).
#   sudo sh connection-limit.sh        replace `ufw limit` with this (or check it is there)
#   sudo sh connection-limit.sh off    back to `ufw limit`
set -eu

RULES=/etc/ufw/before.rules
BEGIN='# sfvr-server: connection limit (connection-limit.sh)'
END='# sfvr-server: connection limit end'
ANCHOR='# End required lines'

if [ "$(id -u)" -ne 0 ]; then echo "run as root (sudo sh connection-limit.sh)"; exit 1; fi
if [ ! -f "$RULES" ]; then echo "$RULES not found: this needs ufw (docs/server-vps.md, step 1)"; exit 1; fi

PORT=5555
if [ -f /etc/sfvr-server/server.env ]; then
  # shellcheck disable=SC1091
  . /etc/sfvr-server/server.env
  PORT=${SFVR_PORT:-5555}
fi

if [ "${1:-on}" = "off" ]; then
  if grep -qF "$BEGIN" "$RULES"; then
    cp "$RULES" "$RULES.sfvr-backup"
    sed -i "/^$BEGIN\$/,/^$END\$/d" "$RULES"
  fi
  ufw delete allow "$PORT/tcp" >/dev/null 2>&1 || true
  ufw limit "$PORT/tcp" comment sfvr-server >/dev/null
  ufw reload >/dev/null
  echo "back to ufw limit on TCP $PORT (6 new connections in 30 s per address)"
  exit 0
fi

if grep -qF "$BEGIN" "$RULES"; then
  if sed -n "/^$BEGIN\$/,/^$END\$/p" "$RULES" | grep -qF -- '! -i lo'; then
    echo "already there: TCP $PORT takes 20 new connections at once and 30 a minute per address (not on the loopback)"
    exit 0
  fi
  # The older rule also limited the loopback (the keeper): replaced below.
  echo "replacing the older rule (it also limited the keeper's connections on the loopback)"
  cp "$RULES" "$RULES.sfvr-backup"
  BACKED_UP=1
  sed -i "/^$BEGIN\$/,/^$END\$/d" "$RULES"
fi
if ! grep -qF "$ANCHOR" "$RULES"; then
  echo "$RULES has no '$ANCHOR' line; add this after its *filter chain lines by hand, then sudo ufw reload:"
  echo "  -A ufw-before-input ! -i lo -p tcp --dport $PORT -m conntrack --ctstate NEW -m hashlimit --hashlimit-name sfvr-rooms --hashlimit-mode srcip --hashlimit-above 30/minute --hashlimit-burst 20 -j DROP"
  exit 1
fi

[ -n "${BACKED_UP:-}" ] || cp "$RULES" "$RULES.sfvr-backup"
tmp=$(mktemp)
awk -v anchor="$ANCHOR" -v begin="$BEGIN" -v end="$END" -v port="$PORT" '
  { print }
  $0 == anchor && !done {
    print begin
    print "-A ufw-before-input ! -i lo -p tcp --dport " port " -m conntrack --ctstate NEW -m hashlimit --hashlimit-name sfvr-rooms --hashlimit-mode srcip --hashlimit-above 30/minute --hashlimit-burst 20 -j DROP"
    print end
    done = 1
  }' "$RULES" > "$tmp"
cat "$tmp" > "$RULES"
rm -f "$tmp"
# The port itself is then allowed: the drop rule above runs first.
ufw delete limit "$PORT/tcp" >/dev/null 2>&1 || true
ufw allow "$PORT/tcp" comment sfvr-server >/dev/null
ufw reload >/dev/null
echo "done: TCP $PORT takes 20 new connections at once and 30 a minute per address ($RULES.sfvr-backup has the old rules)"
