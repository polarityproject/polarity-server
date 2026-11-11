# Polarity rooms API: specification

The rooms API is Polarity's public, read-only view of its custom rooms: which
public rooms are open, who is in them, and how many players are online. It
is meant for websites, Discord bots, stream overlays and other tools. This
document is the contract such tools can rely on.

It is served by a Cloudflare Worker (`cloudflare/rooms-api/src/worker.js`)
from a copy the rooms VPS pushes to it; tools never talk to the VPS. How it
is deployed is in `docs/rooms-vps-docker.md`, "Rooms API".

## Base URL

```
https://polarity-rooms-api.<account>.workers.dev
```

This is the Worker's own address. A custom domain may be put in front of it
later; tools should keep the base URL in one setting.

## At a glance

| Method | Path | What it returns |
| --- | --- | --- |
| `GET` | `/v1/rooms` | Every public room, with its players |
| `GET` | `/v1/summary` | Totals only: players online, public rooms, players in them |
| `GET` | `/v1/health` | `ok` (plain text): the Worker is up (not that the list is fresh) |
| `OPTIONS` | any | CORS preflight, `204` |
| `POST` | `/v1/push` | Internal: the VPS's publisher only (secret). Not for tools |

- `HEAD` works wherever `GET` does.
- A trailing slash is ignored (`/v1/rooms/` is `/v1/rooms`).
- No query parameters are read. There is no paging: the whole list comes in
  one response (at most 2,000 rooms; in practice a few dozen).
- No authentication and no API key for reading.

## Common fields

Both `/v1/rooms` and `/v1/summary` answer a JSON object that starts with:

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | integer | The response format's version: `1`. A breaking change gets a new number (and a new path, `/v2/...`). |
| `updated` | integer | When the rooms server's keeper wrote this list, in Unix milliseconds (UTC). |
| `age` | integer | Seconds between `updated` and when the Worker answered (rounded). |
| `stale` | boolean | `true` when the list is more than 60 seconds old: the VPS, its keeper or the publisher is down. |

**Staleness.** When `stale` is `true`, show the list as old ("last seen 3
minutes ago") or show nothing; never present it as live. `age` is computed
when the Worker builds the response, and Cloudflare may serve that response
from its cache for up to 10 seconds, so `age` can be up to 10 seconds behind.
For the true age, use `updated` against your own clock.

## GET /v1/rooms

```json
{
  "version": 1,
  "updated": 1794106181484,
  "age": 4,
  "stale": false,
  "attribution": "IP Geolocation by DB-IP (https://db-ip.com), CC BY 4.0",
  "rooms": [
    {
      "code": "4SLJEV",
      "title": "Karin user looking for M. Bison!",
      "title_id": 3,
      "title_characters": ["KRN", "VEG"],
      "owner": "POLARITYDEV",
      "client_version": "0.4.0",
      "sfvr_version": "0.4.0",
      "members": 3,
      "capacity": 16,
      "tables": 2,
      "open_seats": 2,
      "rules": "r1 ft2 bo3 t99 ws3",
      "faces": ["RYU", "KEN"],
      "players": [
        {"name": "POLARITYDEV", "country": "DE", "face": "RYU"},
        {"name": "Player 4821", "country": "FR", "face": "KEN"},
        {"name": "DECK", "country": null, "face": null}
      ]
    }
  ]
}
```

With no public rooms open, `rooms` is `[]` (a real answer, not an error).

### `attribution` (string)

The credit for the players' countries. DB-IP's licence (CC BY 4.0) requires
whoever **shows** the countries (flags, country names) to credit them:
display this line, or "IP Geolocation by DB-IP" linked to
https://db-ip.com, where the flags appear (a page footer, an embed's footer).
A tool that does not show countries does not need it.

### A room

| Field | Type | Meaning |
| --- | --- | --- |
| `code` | string | The room's code: 6 characters from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789` (no I, O, 0, 1). Players join it in the game: Custom Rooms, Join with a code. Unique among open rooms; a code can come back after its room closes. |
| `title` | string or `null` | The room's title in English, as the game shows it. `null`: the owner chose none; show something like "POLARITYDEV's room", as the game does. |
| `title_id` | integer | Which title, from the table below (`0` = none). Use it to show the title in another language, or to filter. |
| `title_characters` | array of strings | The character codes a title names, in order: two for title `3`, none otherwise. Already filled into `title`. |
| `owner` | string | The owner's name as the room shows it (see Names). The empty string when the owner just left and the room has not passed on yet. |
| `client_version` | string or `null` | The Polarity version the room plays ("0.4.0"). Only players on that version can list and join it. New in 0.4.0; read `sfvr_version` when it is absent. |
| `sfvr_version` | string or `null` | The same as `client_version`, under its name from before 0.4.0, for readers written then. `null` from a keeper older than versions (none should remain). New in 0.3.0; to be dropped once readers have moved to `client_version`. |
| `members` | integer | Players in the room now (1 to `capacity`). The room's keeper is not counted. |
| `capacity` | integer | The most a room holds: `16`. |
| `tables` | integer | How many tables the room has (1 to 8). |
| `open_seats` | integer | Seats nobody sits in, over all tables whose set has not been called (0 to `2 × tables`). `0` with room left (`members < capacity`) means every seat is taken but players can still join and queue or watch. |
| `rules` | string | The first table's rules (see Rules). Other tables may differ; the API shows only the first. |
| `faces` | array of strings | Up to six character codes, of members in join order, for a quick strip of faces. Members with no character chosen yet are skipped. |
| `players` | array of objects | Every member, in join order (see below). |

**Order.** Rooms come sorted as the game's list sorts them: rooms with open
seats first, then rooms whose tables are full but which have room to join,
then full rooms; within each group, the most members first. Do not rely on
the order for anything else.

### A player

| Field | Type | Meaning |
| --- | --- | --- |
| `name` | string | The name as the room shows it (see Names). |
| `country` | string or `null` | ISO 3166-1 alpha-2 code ("DE", "JP") of the country of the player's connection, looked up by the rooms server in DB-IP's database. `null` when unknown, or when the player turned **Country flag** off in Polarity's settings. A VPN shows its exit's country. Never anything the game itself claims. |
| `face` | string or `null` | The character code the player picked in **My character** (`null`: none yet). `ELV` for Eleven (the fighter it rolls is never shown). |

### Names

- Names are what players set in Polarity (or their Steam name), in UTF-8, up to
  96 bytes, any script: display them as text, never as HTML.
- In public rooms the word filter applies: a name on the server's word list
  appears as `Player` and a number ("Player 4821"), the same number for that
  player for as long as they are in the room. The API always shows the
  filtered name, whatever a player's own word filter setting is.
- Names are not unique. A player has no stable ID in the API, and the same
  person in two rooms (not possible at once) or on two days cannot be
  matched up. Do not try to track individuals.

## GET /v1/summary

```json
{
  "version": 1,
  "updated": 1794106181484,
  "age": 27,
  "stale": false,
  "totals": {"online": 12, "in_public_rooms": 3, "public_rooms": 1}
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `totals.online` | integer | Every player connected to the rooms server: on the hub, in the room list or in any room, private rooms included. Each player counts once (the extra connections a match or a watcher opens are not counted, nor the keeper's own). |
| `totals.in_public_rooms` | integer | The members of all listed rooms (the sum of their `members`). |
| `totals.public_rooms` | integer | How many rooms are listed (the length of `/v1/rooms`' `rooms`). |

Totals cover every Polarity version. Nothing else about private rooms is ever
published: not their number, codes or members.

## Errors

Every error is JSON (`application/json; charset=utf-8`) with an `error` string,
except `/v1/health`.

| Status | Body | When |
| --- | --- | --- |
| `404` | `{"error":"not found","see":["/v1/rooms","/v1/summary"]}` | Any other path |
| `405` | `{"error":"GET only"}` | Another method on a read path |
| `503` | `{"error":"no room list yet"}` | The Worker has never received a list (a fresh deployment). Not cached. Retry later. |
| `5xx` | Cloudflare's own page or JSON | Cloudflare or the Worker failing. Retry with backoff. |

A list that stops updating is **not** an error: the Worker keeps answering
`200` with the last list, `stale: true` and a growing `age`.

## Headers, caching and polling

- `Content-Type: application/json; charset=utf-8` on JSON answers.
- `Access-Control-Allow-Origin: *`: any web page may read the API from the
  browser. Preflights are answered with `Allow-Methods: GET, OPTIONS` and
  cached a day.
- `Cache-Control: public, max-age=10` on `200` answers. Each Cloudflare data
  centre keeps an answer for 10 seconds, so polling faster than every 10
  seconds returns the same answer.
- How fresh the data can be: the keeper writes the list every 5 seconds; the
  VPS pushes it when it changes, and at least every 30 to 60 seconds when it
  does not (a heartbeat, so `stale` stays false). A room opening shows up
  within about 10 to 20 seconds.
- **Polling**: every 15 to 30 seconds is plenty for a page or a bot; never
  more often than every 10. One shared poller for many viewers (a bot polling
  once for every channel) is better than each viewer polling.
- `ETag` and `If-None-Match` are not supported. To tell if anything changed,
  compare `updated`.

## Reference

### Room titles

The titles are the game's Battle Lounge comments. `title_id` is the
comment's number in the game's own table; the game shows them in the
player's language, the API in English.

| `title_id` | `title` (English) |
| --- | --- |
| 0 | none (`title` is `null`) |
| 3 | "{1} user looking for {2}!" with the names of `title_characters[0]` and `[1]` |
| 5 | Hardcore match! Main character only! |
| 6 | Sub-character practice is allowed |
| 7 | Serious match! |
| 8 | Seeking someone stronger than me! |
| 9 | If it's to win, anything goes. |
| 10 | Beginners Welcome! |
| 11 | Anyone will do. |
| 12 | Let's have some casual fun! |
| 13 | Let's hunker down! |
| 14 | Fighting all night! Let's do this! |
| 15 | I don't care if I win or lose! |
| 16 | Random character matches |
| 17 | I'm streaming. |
| 18 | I'm recording commentary |

### Character codes

Used by `faces`, `face` and `title_characters`. The game's own codes, with
the English names the game uses. `ELV` (Eleven) can appear in `faces` and
`face`, never in `title_characters`.

| Code | Name | Code | Name | Code | Name |
| --- | --- | --- | --- | --- | --- |
| RYU | Ryu | VEG | M. Bison | CNL | Chun-Li |
| KEN | Ken | KRN | Karin | ZGF | Zangief |
| DSM | Dhalsim | NSH | Nash | BLR | Vega |
| JRI | Juri | BRD | Birdie | RMK | R. Mika |
| RSD | Rashid | FAN | F.A.N.G | LAR | Laura |
| NCL | Necalli | CMY | Cammy | GUL | Guile |
| IBK | Ibuki | BSN | Balrog | URN | Urien |
| ALX | Alex | Z21 | Akuma | Z20 | Kolin |
| Z22 | Ed | Z23 | Menat | Z24 | Abigail |
| Z25 | Zeku | Z26 | Sakura | Z27 | Blanka |
| Z28 | Falke | Z29 | Cody | Z30 | G |
| Z31 | Sagat | Z32 | Kage | Z33 | Poison |
| Z34 | E. Honda | Z35 | Lucia | Z36 | Gill |
| Z37 | Seth | Z38 | Dan | Z39 | Rose |
| Z40 | Oro | Z41 | Akira | Z42 | Luke |
| ELV | Eleven | | | | |

Note the game's own naming: `VEG` is M. Bison (Dictator) and `BLR` is Vega
(Claw), as in the Japanese release; `BSN` is Balrog (Boxer). A tool should
display the name from this table, never the code.

Character art is not part of the API (it is Capcom's); a tool that wants
pictures brings its own, keyed by code.

### Rules

A table's rules are one line of space-separated fields, always in this
order today, though a reader should not depend on the order:

```
r1 ft2 bo3 t99 ws3
```

| Field | Values | Meaning |
| --- | --- | --- |
| `r1` | `r1` | The rules' format version. A reader that sees another version should show the line as is, or nothing. |
| `ft<N>` | 1 to 20 | First to N games wins the set. |
| `bo<N>` | 1, 3, 5 | Rounds per game: best of N. |
| `t<N>` | 99, 60, 30 | The round timer in seconds. |
| `ws<N>` | N = 3, 5, 0 | Rotation: the winner stays, for at most N sets in a row (0: no limit). |
| `ls<N>` | N = 3, 5, 0 | Rotation: the loser stays, for at most N sets in a row (0: no limit). |
| `rr` | | Rotation: round robin, both players go back to the queue after every set. |

Exactly one of `ws`, `ls` or `rr` appears. The game describes the example
as "First to 2 · winner stays, up to 3 · 99 seconds" and `r1 ft1 bo1 t30 rr`
as "First to 1 · round robin · 30 seconds · one round" (best of 3 is not
mentioned; best of 1 and 5 are: "one round", "best of 5").

Round robin in short: after each set both players go to the back of the
queue, the loser first, unless the loser also played the set before and the
winner did not (then the winner first); the first two in the queue play
next. Three players take turns whoever wins.

## Privacy and fair use

- Only public rooms are listed; private rooms are never shown, and nothing
  identifies a player beyond the name and country a room already shows to
  everyone in it. There are no IP addresses, Steam IDs or account IDs.
- Names can be filtered and countries hidden by the players themselves
  (word filter, Country flag); a tool must not try to undo either.
- Show `stale` lists as old; credit DB-IP where countries are shown.

## Compatibility

- Within version 1, fields may be **added** (like `sfvr_version` in 0.3.0 and `client_version` in 0.4.0),
  never renamed, removed or retyped. Ignore fields you do not know.
- A value's meaning may grow (a new title id, a new character code, a new
  rules field): handle unknown values gracefully (show the raw value, or
  skip it).
- A breaking change gets `"version": 2` and new paths (`/v2/...`); `/v1`
  then keeps working for a while.

## Testing a tool

- The live API's list is often empty. Two ways to get rooms into it:
  - Open public rooms in the game (Custom Rooms, Create a room, Visibility
    Public).
  - Run the Worker locally with a sample list: `cloudflare/rooms-api/test.mjs`
    (`node test.mjs`) shows the push and read calls against a stand-in
    bucket; a tool can use the same sample document as a fixture.
- A fixture for a tool's own tests is any response above; keep one with
  `stale: true`, one with `rooms: []`, and a room whose `title` is `null`,
  with a `null` country and a `null` face.

## For the server side (not for tools)

`POST /v1/push` with `Authorization: Bearer <secret>` and the keeper's list
as the body (at most 512 KB) stores it; it answers `204`, or `401` (secret),
`400` (`not JSON`, `not a room list`) or `413` (too large). The body is the
keeper's `--status` file: the same as a `/v1/rooms` answer without `age`,
`stale` and `attribution`, with `totals` in it. Only the VPS's publisher
(`docker/publisher`) sends it.
