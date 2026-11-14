# polarity-server's rooms server (Weyvelength) in a container, and beside it the
# Cloudflare TURN minter (ice-loop.sh) and the DB-IP country database's
# monthly download (geoip-loop.sh), the same image. Built on the VPS by
# compose.yaml; the guide is docs/install.md.

FROM ubuntu:24.04 AS build
# Keep in step with the mod repository's CMakeLists.txt (FetchContent
# weyvelength) and src/keeper/CMakeLists.txt: the server and the clients speak the
# protocol of the same commit. The mod's patches only change the client; this
# image's own patch (weyvelength-geoip.patch) only the server: members'
# countries from their addresses (--geoip, member data "@cc"; a member's
# "nocc"=1 keeps theirs hidden), and its counts for the keeper (--stats).
# deploy/weyvelength-decode-limit.patch (the mod's third_party copy) caps
# what decoding one message may allocate, on both ends.
# deploy/weyvelength-keepalive.patch (server only) drops a client whose
# network went away without closing its connection within about a minute.
ARG WEYVELENGTH_COMMIT=d4e25c7da7aab18ff82d60d275ea95f794bb2ec9
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates libmaxminddb-dev \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --quiet https://github.com/HeatXD/Weyvelength.git /src \
 && git -C /src checkout --quiet "$WEYVELENGTH_COMMIT" \
 && git -C /src submodule update --init --recursive --quiet
COPY docker/weyvelength-geoip.patch deploy/weyvelength-decode-limit.patch deploy/weyvelength-keepalive.patch /tmp/
RUN git -C /src apply /tmp/weyvelength-geoip.patch \
 && git -C /src apply /tmp/weyvelength-decode-limit.patch \
 && git -C /src apply /tmp/weyvelength-keepalive.patch
RUN cmake -S /src -B /src/build -DCMAKE_BUILD_TYPE=Release -DWEYVELENGTH_BUILD_TESTS=OFF >/dev/null \
 && cmake --build /src/build --target Server -j "$(nproc)"

FROM ubuntu:24.04
# curl: the TURN minter's request to Cloudflare and the database download;
# libmaxminddb0: the server's country lookups.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl libmaxminddb0 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --home-dir /data --shell /usr/sbin/nologin sfvr \
 && install -d -o 10001 -g 10001 -m 0750 /data \
 && install -d -o 10001 -g 10001 -m 0755 /geoip /stats
COPY --from=build /src/build/Server/Server /usr/local/bin/weyvelength-server
COPY --from=build /src/docs/examples/mint-cloudflare-credentials.sh /usr/local/bin/
COPY docker/server-start.sh docker/ice-loop.sh docker/geoip-loop.sh /usr/local/bin/
RUN chmod 0755 /usr/local/bin/weyvelength-server /usr/local/bin/*.sh
USER 10001:10001
WORKDIR /data
VOLUME /data
ENTRYPOINT ["/usr/local/bin/server-start.sh"]
