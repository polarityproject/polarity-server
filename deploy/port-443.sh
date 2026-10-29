#!/bin/sh
# The rooms server on TCP 443 as well, for players whose network lets only
# common ports through (the mod tries 443 alongside 5555 for the public
# server). A ufw NAT rule redirects 443 to the server's own port, so both
# reach the same process and the same rooms; the firewall's existing
# `limit` rule for that port covers the redirected connections too.
#   sudo sh port-443.sh        add the redirect (or check it is there)
#   sudo sh port-443.sh off    remove it
set -eu

RULES=/etc/ufw/before.rules
BEGIN='# sfvr-server: TCP 443 to the rooms server (port-443.sh)'
END='# sfvr-server: end'

if [ "$(id -u)" -ne 0 ]; then echo "run as root (sudo sh port-443.sh)"; exit 1; fi
if [ ! -f "$RULES" ]; then echo "$RULES not found: this needs ufw (docs/server-vps.md, step 1)"; exit 1; fi

PORT=5555
if [ -f /etc/sfvr-server/server.env ]; then
  # shellcheck disable=SC1091
  . /etc/sfvr-server/server.env
  PORT=${SFVR_PORT:-5555}
fi

if [ "${1:-on}" = "off" ]; then
  if ! grep -qF "$BEGIN" "$RULES"; then echo "no redirect to remove"; exit 0; fi
  cp "$RULES" "$RULES.sfvr-backup"
  sed -i "/^$BEGIN\$/,/^$END\$/d" "$RULES"
  ufw reload
  echo "removed: 443 no longer reaches the rooms server ($RULES.sfvr-backup has the old rules)"
  exit 0
fi

if grep -qF "$BEGIN" "$RULES"; then
  echo "already there: TCP 443 goes to port $PORT"
  exit 0
fi
# Something else on 443 (a web server) would lose its traffic to the redirect.
if ss -Hltn 'sport = :443' | grep -q .; then
  echo "something already listens on TCP 443:"; ss -ltnp 'sport = :443'
  echo "not adding the redirect; free the port, or leave the server on $PORT only"
  exit 1
fi
# A *nat section of someone else's: merge by hand rather than writing a second one.
if grep -q '^\*nat' "$RULES"; then
  echo "$RULES already has a *nat section; add this line to its PREROUTING rules by hand, then sudo ufw reload:"
  echo "  -A PREROUTING -p tcp --dport 443 -j REDIRECT --to-ports $PORT"
  exit 1
fi

cp "$RULES" "$RULES.sfvr-backup"
# The nat table goes before ufw's own *filter section, at the top.
tmp=$(mktemp)
{
  echo "$BEGIN"
  echo '*nat'
  echo ':PREROUTING ACCEPT [0:0]'
  echo "-A PREROUTING -p tcp --dport 443 -j REDIRECT --to-ports $PORT"
  echo 'COMMIT'
  echo "$END"
  cat "$RULES"
} > "$tmp"
cat "$tmp" > "$RULES"
rm -f "$tmp"
ufw reload
echo "done: TCP 443 goes to the rooms server on port $PORT ($RULES.sfvr-backup has the old rules)"
echo "check from a PC: Test-NetConnection <this server's address> -Port 443 (PowerShell)"
