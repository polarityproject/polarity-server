#include "keeper/keeper_internal.h"

namespace sfv::keeper {

using namespace internal;

void Keeper::Close(Room& room, const char* why) {
    if (room.closing) return;
    stats_.roomsClosed++;
    Log("keeper: closing room %s: %s", room.code.empty() ? "(new)" : room.code.c_str(), why);
    room.closing = true;
    for (auto& r : room.relays) EndRelay(room, *r, "the room closed");
    room.relays.clear();
    if (room.requester && lobby_) {
        Answer(*lobby_, room.requester, {room.requestSeq, "room", false, "the room could not be made"}, now_);
        room.requester = 0;
    }
    connector_->Cancel(room.connecting);
    room.connecting = 0;
    if (room.listedOn) listRev_++;
    room.listedOn = false;
    if (room.client) Retire(room.client);
}

void Keeper::StartRelay(Room& room, int table, int64_t nowMs) {
    if (table < 0 || table >= static_cast<int>(room.tables.size())) return;
    Table& t = room.tables[table];
    auto r = std::make_unique<RelayLink>();
    r->table = table;
    r->matchCode = t.s.code;
    r->startedMs = nowMs;
    r->statsMs = nowMs;
    Ask(r->downConnecting, r->downAskedMs, nowMs);
    if (!t.build.empty()) {
        r->data.emplace_back("build", t.build);
        r->data.emplace_back("protocol", std::to_string(lobby::kProtocol));
    }
    RelayConfig cfg;
    cfg.firstTo = t.setRules.firstTo;
    cfg.maxViewers = kRoomCap;
    const std::string tag = room.code + " table " + std::to_string(table + 1);
    cfg.log = [this, tag](const std::string& line) { Log("keeper: room %s: %s", tag.c_str(), line.c_str()); };
    RelayLink* raw = r.get();
    r->core = std::make_unique<SpectateRelay>(
        cfg, [raw](uint64_t to, const lobby::Message& m) { SendLobby(raw->up, to, m); },
        [raw](uint64_t to, const lobby::Message& m) { SendLobby(raw->down, to, m); });
    if (!t.build.empty()) r->core->SetBuild(std::strtoull(t.build.c_str(), nullptr, 16));
    else r->upStartedMs = -2;
    stats_.relays++;
    Log("keeper: room %s: table %d: relay for match room %s", room.code.c_str(), table + 1, r->matchCode.c_str());
    room.relays.push_back(std::move(r));
}

void Keeper::PumpRelays(Room& room, int64_t nowMs) {
    for (auto& r : room.relays) {
        if (r->ended) continue;
        if (r->downConnecting) {
            WeyveClient* c = nullptr;
            if (Arrived(r->downConnecting, r->downAskedMs, nowMs, &c)) {
                if (!c) {
                    Log("keeper: room %s: table %d: no relay (cannot reach the server)", room.code.c_str(), r->table + 1);
                    EndRelay(room, *r, "cannot reach the server");
                    continue;
                }
                r->down = c;
                weyve_create_room(r->down);
            }
        }
        if (r->upStartedMs < 0 && (r->core->Stats().viewers > 0 || r->upStartedMs == -2)) {
            r->upStartedMs = nowMs;
            Ask(r->upConnecting, r->upAskedMs, nowMs);
        }
        if (r->upConnecting) {
            WeyveClient* c = nullptr;
            if (Arrived(r->upConnecting, r->upAskedMs, nowMs, &c)) {
                if (c) {
                    r->up = c;
                    weyve_join_room(r->up, r->matchCode.c_str(), nullptr);
                    Log("keeper: room %s: table %d: a viewer; the relay joins match room %s", room.code.c_str(), r->table + 1,
                        r->matchCode.c_str());
                } else {
                    r->core->HostGone(nowMs);
                }
            }
        }
        PumpRelayUp(room, *r, nowMs);
        PumpRelayDown(room, *r, nowMs);
        if (r->ended) continue;
        r->core->Step(nowMs);
        if (nowMs - r->statsMs >= 60000) {
            r->statsMs = nowMs;
            const RelayStats s = r->core->Stats();
            Log("keeper: room %s: table %d: relay %s: %zu viewers, %zu games, %llu KB from the host, %llu KB to viewers",
                room.code.c_str(), r->table + 1, r->relayCode.c_str(), s.viewers, s.games,
                static_cast<unsigned long long>(s.bytesFromHost / 1024), static_cast<unsigned long long>(s.bytesToViewers / 1024));
            if (s.viewers) Log("keeper: room %s: table %d: relay %s viewers: %s", room.code.c_str(), r->table + 1, r->relayCode.c_str(),
                               r->core->ViewersLine().c_str());
        }
        const bool setOn = r->table >= 0 && r->table < static_cast<int>(room.tables.size()) &&
                           room.tables[r->table].s.code == r->matchCode &&
                           (room.tables[r->table].s.phase == TablePhase::Playing || room.tables[r->table].s.phase == TablePhase::Connecting);
        if (r->core->Done(nowMs)) EndRelay(room, *r, "done");
        else if (r->upStartedMs >= 0 && !r->core->Stats().hostAccepted && nowMs - r->upStartedMs > 60000) EndRelay(room, *r, "the host never answered");
        else if (r->upStartedMs < 0 && !setOn) EndRelay(room, *r, "the set is over; nobody watched");
    }
    std::erase_if(room.relays, [](const std::unique_ptr<RelayLink>& r) { return r->ended; });
}

void Keeper::PumpRelayUp(Room& room, RelayLink& r, int64_t nowMs) {
    (void)room;
    WeyveClient* c = r.up;
    if (!c) return;
    if (!weyve_poll(c)) {
        r.core->HostGone(nowMs);
        weyve_client_destroy(c);
        r.up = nullptr;
        return;
    }
    const uint32_t self = weyve_id(c);
    WeyveEvent e;
    while (weyve_next(c, &e)) {
        switch (e.type) {
            case WEYVE_EVENT_ROOM_ID_ASSIGNED: {
                r.upIn = true;
                const std::string message = ProofMessage(r.matchCode, self);
                uint8_t signature[64];
                if (Sign(o_.key, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature)) {
                    weyve_set_member_data(c, kProof, Base64Encode(signature, sizeof(signature)).c_str());
                }
                break;
            }
            case WEYVE_EVENT_ROOM_ERROR:
                if (!r.upIn) {
                    Log("keeper: relay: cannot join match room %s: %s", r.matchCode.c_str(), RoomErrorText(e.data.room_error.code));
                    r.core->HostGone(nowMs);
                }
                break;
            case WEYVE_EVENT_PEER_LEFT:
                if (e.data.peer_left.id == self || (r.hostId && e.data.peer_left.id == r.hostId)) r.core->HostGone(nowMs);
                break;
            case WEYVE_EVENT_KICKED:
            case WEYVE_EVENT_BANNED:
                Log("keeper: relay: removed from match room %s", r.matchCode.c_str());
                r.core->HostGone(nowMs);
                break;
            default:
                break;
        }
    }
    if (r.upIn && !r.hostSet) {
        uint32_t len = 0;
        const char* build = weyve_room_data(c, "build", &len);
        const uint32_t host = weyve_host_id(c);
        if (build && len && host && host != self) {
            r.hostId = host;
            r.hostSet = true;
            r.core->SetHost(host, std::strtoull(Str(build, len).c_str(), nullptr, 16));
            r.data.clear();
            for (const char* key : {"build", "version", "game", "protocol"}) {
                uint32_t n = 0;
                const char* v = weyve_room_data(c, key, &n);
                if (v) r.data.emplace_back(key, Str(v, n));
            }
            if (r.published && r.down) {
                for (const auto& [key, value] : r.data) weyve_set_room_data(r.down, key.c_str(), value.c_str());
            }
        }
    }
    uint32_t from = 0, len = 0;
    while (const uint8_t* d = weyve_next_p2p(c, &from, &len)) {
        if (len < 2 || d[0] != kControlChannel || from != r.hostId) continue;
        if (auto m = lobby::Decode(d + 1, len - 1)) r.core->FromHost(*m, nowMs);
    }
}

void Keeper::PumpRelayDown(Room& room, RelayLink& r, int64_t nowMs) {
    WeyveClient* c = r.down;
    if (!c) return;
    if (!weyve_poll(c)) {
        EndRelay(room, r, "lost the server");
        return;
    }
    const uint32_t self = weyve_id(c);
    WeyveEvent e;
    while (weyve_next(c, &e)) {
        switch (e.type) {
            case WEYVE_EVENT_ROOM_ID_ASSIGNED:
                r.downIn = true;
                r.relayCode = Str(e.data.room_assigned.id, e.data.room_assigned.id_len);
                break;
            case WEYVE_EVENT_ROOM_ERROR:
                if (!r.downIn) {
                    EndRelay(room, r, "cannot make its room");
                    return;
                }
                break;
            case WEYVE_EVENT_PEER_LEFT:
                if (e.data.peer_left.id != self) r.core->ViewerGone(e.data.peer_left.id);
                break;
            case WEYVE_EVENT_CHAT:
                if (e.data.chat.from != self) weyve_ban_member(c, e.data.chat.from);
                break;
            default:
                break;
        }
    }
    if (r.downIn && !r.data.empty() && !r.published) {
        for (const auto& [key, value] : r.data) weyve_set_room_data(c, key.c_str(), value.c_str());
        r.published = true;
        if (r.table >= 0 && r.table < static_cast<int>(room.tables.size())) {
            Table& t = room.tables[r.table];
            if (t.s.code == r.matchCode && (t.s.phase == TablePhase::Playing || t.s.phase == TablePhase::Connecting)) {
                t.s.watch = r.relayCode;
                WriteTables(room);
            }
        }
        Log("keeper: room %s: table %d: relay room %s is up", room.code.c_str(), r.table + 1, r.relayCode.c_str());
    }
    uint32_t from = 0, len = 0;
    while (const uint8_t* d = weyve_next_p2p(c, &from, &len)) {
        if (len < 2 || d[0] != kControlChannel) continue;
        if (auto m = lobby::Decode(d + 1, len - 1)) r.core->FromViewer(from, *m, nowMs);
    }
}

void Keeper::EndRelay(Room& room, RelayLink& r, const char* why) {
    if (r.ended) return;
    r.ended = true;
    const RelayStats s = r.core ? r.core->Stats() : RelayStats{};
    Log("keeper: room %s: table %d: relay %s ends (%s): %zu games, %llu KB from the host, %llu KB to viewers",
        room.code.c_str(), r.table + 1, r.relayCode.c_str(), why, s.games, static_cast<unsigned long long>(s.bytesFromHost / 1024),
        static_cast<unsigned long long>(s.bytesToViewers / 1024));
    connector_->Cancel(r.upConnecting);
    connector_->Cancel(r.downConnecting);
    r.upConnecting = r.downConnecting = 0;
    if (r.up) Retire(r.up);
    if (r.down) Retire(r.down);
    if (r.table >= 0 && r.table < static_cast<int>(room.tables.size()) && !r.relayCode.empty() &&
        room.tables[r.table].s.watch == r.relayCode) {
        room.tables[r.table].s.watch.clear();
        if (!room.closing) WriteTables(room);
    }
}

} // namespace sfv::keeper
