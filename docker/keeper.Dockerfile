# sfvr-keeper (custom rooms' tables and moderation) in a container: a client
# of the rooms server beside it. Built on the VPS by compose.yaml from the
# sources in deploy/keeper (the mod repository's scripts/keeper-bundle.ps1
# copies them there); the guide is docs/rooms-vps-docker.md.

FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates libssl-dev \
 && rm -rf /var/lib/apt/lists/*
COPY deploy/keeper /src
# Fetches the same Weyvelength commit as the server and applies the mod's
# client patches (src/keeper/CMakeLists.txt).
RUN cmake -S /src/src/keeper -B /build -DCMAKE_BUILD_TYPE=Release >/dev/null \
 && cmake --build /build -j "$(nproc)"

FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libssl3t64 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --home-dir /data --shell /usr/sbin/nologin sfvr \
 && install -d -o 10001 -g 10001 -m 0750 /data
COPY --from=build /build/sfvr-keeper /usr/local/bin/sfvr-keeper
COPY --from=build /src/SOURCE.txt /usr/local/share/sfvr-keeper-source.txt
USER 10001:10001
WORKDIR /data
VOLUME /data
ENTRYPOINT ["/usr/local/bin/sfvr-keeper"]
