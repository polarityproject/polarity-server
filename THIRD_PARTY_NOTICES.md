# Third-party notices

Some files in this repository, and some code in the images it builds, come
from other projects. Each keeps its own license, listed here.

## In this repository

### Weyvelength patches

These files change [Weyvelength](https://github.com/HeatXD/Weyvelength), the
rooms server, and contain parts of its source:

- `deploy/weyvelength-*.patch`
- `docker/weyvelength-geoip.patch`
- `deploy/keeper/third_party/*.patch`

Weyvelength is copyright (c) 2026, Jamie Meyer & Contributors, under the
BSD 2-Clause License. Its full text is at the end of this file.

### Word filter list

`docker/keeper-words.default.txt` holds the English list of
[List of Dirty, Naughty, Obscene, and Otherwise Bad Words](https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words)
by the LDNOOBW contributors, under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The list is
unchanged; the header above it is ours.

### Street Fighter V text

Street Fighter V and its characters are Capcom's. Two parts of this
repository repeat the game's own text so that rooms read the way the game
shows them:

- The room titles are the game's English Battle Lounge comments, in
  `deploy/keeper/src/core/keeper_protocol.cpp` and `docs/rooms-api.md`.
- The character names and codes are the game's roster, in the same files.

This text belongs to Capcom and is outside this repository's own license.
Polarity is an unofficial project and is not affiliated with Capcom.

## In the built images

The Dockerfiles in `docker/` build the server and the keeper from source.
The images then contain this code:

| Component | Image | License | Copyright |
| --- | --- | --- | --- |
| [Weyvelength](https://github.com/HeatXD/Weyvelength) (commit d4e25c7) | server, keeper | BSD 2-Clause | Jamie Meyer & Contributors |
| [asio](https://github.com/chriskohlhoff/asio) | server, keeper | Boost Software License 1.0 | Christopher M. Kohlhoff |
| [spdlog](https://github.com/gabime/spdlog) and its bundled [{fmt}](https://github.com/fmtlib/fmt) | server, keeper | MIT | Gabi Melman and spdlog contributors; Victor Zverovich and {fmt} contributors |
| [zpp_bits](https://github.com/eyalz800/zpp_bits) | server, keeper | MIT | Eyal Z |
| [libjuice](https://github.com/paullouisageneau/libjuice) | keeper | Mozilla Public License 2.0 | Paul-Louis Ageneau |
| [libmaxminddb](https://github.com/maxmind/libmaxminddb) (Ubuntu package) | server | Apache License 2.0 | MaxMind, Inc. |
| [OpenSSL](https://www.openssl.org) 3 (Ubuntu package) | keeper | Apache License 2.0 | The OpenSSL Project Authors |

libjuice's source, unmodified, is at the link above.

The base images (Ubuntu, Alpine with Python) and the packages they install
keep their own licenses. The monitoring stack in
`compose.monitoring.yaml` runs the published Prometheus, Loki, Alloy,
Grafana and node-exporter images as they are.

## Data fetched at run time

The `geoip` service downloads DB-IP's free IP to Country Lite database each
month. It is under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
The rooms API credits it as "IP Geolocation by DB-IP (https://db-ip.com)"
next to the players' countries, and tools that show those countries should
credit it too.

## Weyvelength license

```
BSD 2-Clause License

Copyright (c) 2026, Jamie Meyer & Contributors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
