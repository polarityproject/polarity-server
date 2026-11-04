# sfvr-server's rooms server (Weyvelength) in a container, and the Cloudflare
# TURN minter beside it (the same image, ice-loop.sh). Built on the VPS by
# compose.yaml; the guide is docs/rooms-vps-docker.md.

FROM ubuntu:24.04 AS build
# Keep in step with the mod repository's CMakeLists.txt (FetchContent
# weyvelength) and deploy/install.sh: the server and the clients speak the
# protocol of the same commit. The mod's patches only change the client.
ARG WEYVELENGTH_COMMIT=d4e25c7da7aab18ff82d60d275ea95f794bb2ec9
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --quiet https://github.com/HeatXD/Weyvelength.git /src \
 && git -C /src checkout --quiet "$WEYVELENGTH_COMMIT" \
 && git -C /src submodule update --init --recursive --quiet
RUN cmake -S /src -B /src/build -DCMAKE_BUILD_TYPE=Release -DWEYVELENGTH_BUILD_TESTS=OFF >/dev/null \
 && cmake --build /src/build --target Server -j "$(nproc)"

FROM ubuntu:24.04
# curl: the TURN minter's request to Cloudflare.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --home-dir /data --shell /usr/sbin/nologin sfvr \
 && install -d -o 10001 -g 10001 -m 0750 /data
COPY --from=build /src/build/Server/Server /usr/local/bin/weyvelength-server
COPY --from=build /src/docs/examples/mint-cloudflare-credentials.sh /usr/local/bin/
COPY docker/server-start.sh docker/ice-loop.sh /usr/local/bin/
RUN chmod 0755 /usr/local/bin/weyvelength-server /usr/local/bin/*.sh
USER 10001:10001
WORKDIR /data
VOLUME /data
ENTRYPOINT ["/usr/local/bin/server-start.sh"]
