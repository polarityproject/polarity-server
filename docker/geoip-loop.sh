#!/bin/sh
# DB-IP's free IP-to-country database ("IP to Country Lite", CC BY 4.0:
# "IP Geolocation by DB-IP", https://db-ip.com) for the rooms server's
# --geoip, in its own container. This month's file (last month's until this
# month's is out, on the 1st), looked for once a day, unpacked beside and
# renamed over /geoip/country.mmdb; the server reopens it when it changes. A
# failed download leaves the file it has.
set -u
DIR=/geoip
while :; do
	have=$(cat "$DIR/month" 2>/dev/null || true)
	this=$(date -u +%Y-%m)
	last=$(date -u -d "$(date -u +%Y-%m-15) -1 month" +%Y-%m)
	for month in "$this" "$last"; do
		[ "$have" = "$month" ] && break # what we have is the newest there is
		url="https://download.db-ip.com/free/dbip-country-lite-$month.mmdb.gz"
		if curl -fsS --max-time 300 -o "$DIR/download.gz" "$url" \
			&& gzip -dc "$DIR/download.gz" > "$DIR/country.mmdb.new" && [ -s "$DIR/country.mmdb.new" ]; then
			mv -f "$DIR/country.mmdb.new" "$DIR/country.mmdb"
			echo "$month" > "$DIR/month"
			rm -f "$DIR/download.gz"
			echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) DB-IP country database $month"
			break
		fi
		rm -f "$DIR/download.gz" "$DIR/country.mmdb.new"
		[ "$month" = "$this" ] && echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) no DB-IP database for $month yet; trying $last"
	done
	sleep 86400
done
