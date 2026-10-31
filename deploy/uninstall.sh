#!/bin/sh
# Removes everything install.sh added for sfvr-server: the services, the
# user and its /opt, /etc and /var/lib folders. Nothing else on the machine
# is touched (the build packages stay). Run as root: sudo sh uninstall.sh
set -eu

NAME=sfvr-server

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo sh $0" >&2
	exit 1
fi

systemctl disable --now "$NAME.service" "$NAME-ice.timer" sfvr-reports.service 2>/dev/null || true
systemctl stop "$NAME-ice.service" 2>/dev/null || true
rm -f "/etc/systemd/system/$NAME.service" "/etc/systemd/system/$NAME-ice.service" "/etc/systemd/system/$NAME-ice.timer" \
	/etc/systemd/system/sfvr-reports.service
systemctl daemon-reload
rm -rf "/opt/$NAME" "/etc/$NAME" "/var/lib/$NAME"
id "$NAME" >/dev/null 2>&1 && userdel "$NAME"
echo "$NAME removed (firewall rules are yours to remove: sudo ufw status numbered)"
