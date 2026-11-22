#include "keeper/keeper_internal.h"

namespace sfv::keeper {

using namespace internal;

void Keeper::FindLobby(Room& room) {
    const std::string build = std::to_string(o_.protocol);
    const WeyveRoomFilter filters[] = {{kKind, WEYVE_FILTER_EQUALS, kKindLobby}, {kBuild, WEYVE_FILTER_EQUALS, build.c_str()}};
    room.lobbyListAsked = weyve_list_rooms(room.client, filters, 2);
    if (!room.lobbyListAsked) weyve_create_room(room.client);
}

void Keeper::OnLobbyList(Room& room) {
    room.lobbyListAsked = false;
    std::string best;
    uint32_t bestCount = 0;
    const uint32_t n = weyve_room_list_count(room.client);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t len = 0;
        const char* id = weyve_room_list_id(room.client, i, &len);
        const uint32_t members = weyve_room_list_members(room.client, i);
        if (!id || !weyve_room_list_joinable(room.client, i) || members >= static_cast<uint32_t>(kLobbyCap)) continue;
        if (best.empty() || members > bestCount) {
            best = Str(id, len);
            bestCount = members;
        }
    }
    if (best.empty()) {
        weyve_create_room(room.client);
        return;
    }
    Log("keeper: adopting lobby channel %s (%u members, no keeper of its own)", best.c_str(), bestCount);
    room.code = best;
    weyve_join_room(room.client, best.c_str(), nullptr);
}

void Keeper::OnInRoom(Room& room, const std::string& code, int64_t nowMs) {
    WeyveClient* c = room.client;
    const uint32_t self = weyve_id(c);
    room.code = code;
    room.inRoom = true;
    room.host = weyve_is_host(c);
    const std::string message = ProofMessage(code, self);
    uint8_t signature[64];
    if (Sign(o_.key, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature)) {
        weyve_set_member_data(c, kProof, Base64Encode(signature, sizeof(signature)).c_str());
    } else {
        Log("keeper: cannot sign the proof for room %s", code.c_str());
    }
    uint32_t count = 0;
    const uint32_t* ids = weyve_members(c, &count);
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i] == self || room.members.count(ids[i])) continue;
        uint32_t len = 0;
        const char* name = weyve_peer_name(c, ids[i], &len);
        OnJoined(room, ids[i], Str(name, len), nowMs);
        const char* chara = weyve_member_data(c, ids[i], "ch", &len);
        if (chara) room.members[ids[i]].chara = Str(chara, len).substr(0, 3);
    }
    if (room.kind == Kind::Lobby) {
        Log("keeper: lobby channel %s (%s, %zu members)", code.c_str(), room.host ? "host" : "waiting for host",
            room.members.size());
    } else {
        if (room.reclaimed) ReadBackCustom(room);
        if (room.closing) return;
        Log("keeper: custom room %s, %s, %d tables, %s (%s)", code.c_str(), FormatTitle(room.settings.title).c_str(),
            room.settings.tables, room.settings.isPublic ? "public" : "private", room.host ? "host" : "waiting for host");
        if (room.requester && lobby_) {
            Answer(*lobby_, room.requester, {room.requestSeq, "room", true, code}, nowMs);
            room.requester = 0;
        }
    }
    if (room.host) SetUpAsHost(room, nowMs);
    SaveState();
}

void Keeper::ReadBackCustom(Room& room) {
    uint32_t len = 0;
    auto data = [&](const char* key) {
        const char* v = weyve_room_data(room.client, key, &len);
        return Str(v, len);
    };
    if (data(kKind) != kKindCustom) {
        Log("keeper: room %s is not a custom room (kind '%s'); leaving it", room.code.c_str(), data(kKind).c_str());
        Close(room, "not ours");
        return;
    }
    room.settings.tables = std::clamp(std::atoi(data(kTables).c_str()), 1, kMaxTables);
    room.settings.isPublic = data(kPublic) == "1";
    room.version = data(kVersion).substr(0, kVersionMax);
    if (!ParseTitle(data(kTitle), &room.settings.title)) room.settings.title = RoomTitle{};
    room.owner = static_cast<uint32_t>(std::strtoul(data(kOwner).c_str(), nullptr, 10));
    if (!room.members.count(room.owner)) {
        room.owner = NextOwner(room.order, 0, weyve_id(room.client));
    }
}

void Keeper::SetUpAsHost(Room& room, int64_t nowMs) {
    WeyveClient* c = room.client;
    room.written.clear();
    room.listed.clear();
    room.listedOn = false;
    if (room.kind == Kind::Lobby) {
        weyve_set_room_listed(c, true);
        weyve_set_room_listing(c, kKind, kKindLobby);
        room.listed[kKind] = kKindLobby;
        WriteData(room, kKind, kKindLobby);
        WriteData(room, kBuild, std::to_string(o_.protocol));
        listWrittenMs_ = -1;
    } else {
        weyve_set_room_listed(c, false);
        WriteData(room, kKind, kKindCustom);
        WriteData(room, kBuild, std::to_string(o_.protocol));
        WriteData(room, kTitle, FormatTitle(room.settings.title));
        if (!room.version.empty()) WriteData(room, kVersion, room.version);
        WriteData(room, kPublic, room.settings.isPublic ? "1" : "0");
        WriteData(room, kTables, std::to_string(room.settings.tables));
        for (int t = 0; t < room.settings.tables; t++) {
            uint32_t len = 0;
            const char* kept = weyve_room_data(c, TableKey(t).c_str(), &len);
            Rules rules;
            if (!kept || !ParseRules(Str(kept, len), &rules)) rules = Rules{};
            WriteData(room, TableKey(t), FormatRules(rules));
        }
        if (room.owner) WriteData(room, kOwner, std::to_string(room.owner));
        weyve_set_room_joinable(c, Count(room) < kRoomCap);
        WriteWordList(room);
        room.masked.clear();
        room.maskNumbers.clear();
        for (uint32_t id : room.order) CheckName(room, id);
        WriteMask(room);
        SetUpTables(room, nowMs);
    }
    room.setUp = true;
}

void Keeper::OnJoined(Room& room, uint32_t id, const std::string& name, int64_t nowMs) {
    if (room.members.count(id)) return;
    Member& m = room.members[id];
    m.id = id;
    m.name = name;
    m.joinedMs = nowMs;
    if (std::find(room.order.begin(), room.order.end(), id) == room.order.end()) room.order.push_back(id);
    if (room.kind == Kind::Custom) {
        Log("keeper: room %s: %s (%u) joined, %d members", room.code.c_str(), name.c_str(), id, Count(room));
        if (id == room.owner) room.ownerArrived = true;
        room.emptySinceMs = -1;
        CheckName(room, id);
        if (room.host && Count(room) > kRoomCap) {
            Log("keeper: room %s is full: removing %u", room.code.c_str(), id);
            weyve_kick_member(room.client, id);
        } else if (room.host && Count(room) >= kRoomCap) {
            weyve_set_room_joinable(room.client, false);
        }
    }
}

void Keeper::OnLeft(Room& room, uint32_t id, int64_t nowMs) {
    const auto it = room.members.find(id);
    if (it == room.members.end()) return;
    const std::string name = it->second.name;
    room.members.erase(it);
    std::erase(room.order, id);
    std::erase_if(room.outbox, [&](const Outgoing& o) { return o.to == id; });
    if (room.kind != Kind::Custom) return;
    Log("keeper: room %s: %s (%u) left, %d members", room.code.c_str(), name.c_str(), id, Count(room));
    std::erase(room.masked, id);
    room.maskNumbers.erase(id);
    WriteMask(room);
    OffTables(room, id, nowMs);
    if (id == room.owner) {
        room.owner = NextOwner(room.order, id, weyve_id(room.client));
        if (room.owner) {
            Log("keeper: room %s: %s (%u) owns it now", room.code.c_str(), room.members[room.owner].name.c_str(), room.owner);
            if (room.host) WriteData(room, kOwner, std::to_string(room.owner));
        }
    }
    if (room.host && Count(room) < kRoomCap) weyve_set_room_joinable(room.client, true);
    if (room.members.empty()) room.emptySinceMs = nowMs;
}

void Keeper::OnMemberData(Room& room, uint32_t id, const std::string& key, const std::string& value, int64_t nowMs) {
    auto it = room.members.find(id);
    if (it == room.members.end()) {
        if (key == kRequest && !value.empty() && room.kind == Kind::Custom)
            Log("keeper: room %s: a request from %u, who has not joined here yet: %s", room.code.c_str(), id, value.c_str());
        return;
    }
    Member& m = it->second;
    if (key == "ch") {
        m.chara = value.substr(0, 3);
    } else if (key == "@cc") {
        const bool ok = value.size() == 2 && std::isupper(static_cast<unsigned char>(value[0])) &&
                        std::isupper(static_cast<unsigned char>(value[1]));
        m.country = ok ? value : std::string();
    } else if (key == kVersion) {
        m.version = value.substr(0, kVersionMax);
        if (room.kind == Kind::Custom && !room.version.empty() && !m.version.empty() && m.version != room.version)
            Log("keeper: room %s: %s (%u) runs Polarity %s, the room %s", room.code.c_str(), m.name.c_str(), id, m.version.c_str(),
                room.version.c_str());
    } else if (key == kAck) {
        const uint32_t seq = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        std::erase_if(room.outbox, [&](const Outgoing& o) { return o.to == id && o.seq <= seq; });
    } else if (key == kRequest && !value.empty()) {
        Request r;
        if (!ParseRequest(value, &r)) {
            Log("keeper: room %s: member %u sent a request that does not parse", room.code.c_str(), id);
            return;
        }
        if (r.seq <= m.lastSeq) return;
        m.lastSeq = r.seq;
        OnRequest(room, m, r, nowMs);
    }
}

void Keeper::OnRequest(Room& room, Member& m, const Request& request, int64_t nowMs) {
    {
        static const char* const kKinds[] = {"peek", "room", "seat", "stand", "queue", "unqueue", "ready", "decline",
                                             "code", "score", "result", "forfeit", "again", "rules", "kick"};
        const auto known = std::find(std::begin(kKinds), std::end(kKinds), request.kind);
        stats_.requests[known != std::end(kKinds) ? request.kind : std::string("other")]++;
    }
    if (room.kind == Kind::Lobby && request.kind == "peek") {
        Peek(room, m, request, nowMs);
        return;
    }
    if (room.kind == Kind::Lobby && request.kind == "room") {
        RoomSettings settings;
        if (!ParseRoomSettings(request, &settings)) {
            Answer(room, m.id, {request.seq, "room", false, "bad room settings"}, nowMs);
            return;
        }
        if (m.lastRoomAskMs >= 0 && nowMs - m.lastRoomAskMs < kRoomAskEveryMs) {
            Answer(room, m.id, {request.seq, "room", false, "wait a moment before making another room"}, nowMs);
            return;
        }
        if (static_cast<int>(rooms_.size()) >= o_.maxRooms) {
            Answer(room, m.id, {request.seq, "room", false, "the server has too many rooms; try again later"}, nowMs);
            return;
        }
        m.lastRoomAskMs = nowMs;
        auto r = std::make_unique<Room>();
        r->kind = Kind::Custom;
        r->settings = settings;
        r->version = m.version;
        r->owner = m.id;
        r->requester = m.id;
        r->requestSeq = request.seq;
        r->createdMs = nowMs;
        Log("keeper: %s (%u) asks for a room: %s, %d tables, %s, Polarity %s", m.name.c_str(), m.id, FormatTitle(settings.title).c_str(),
            settings.tables, settings.isPublic ? "public" : "private", m.version.empty() ? "?" : m.version.c_str());
        Ask(r->connecting, r->connectAskedMs, nowMs);
        stats_.roomsAsked++;
        rooms_.push_back(std::move(r));
        return;
    }
    if (room.kind == Kind::Custom) {
        TableRequest(room, m, request, nowMs);
        return;
    }
    Answer(room, m.id, {request.seq, request.kind, false, "unknown request"}, nowMs);
}

void Keeper::Answer(Room& room, uint32_t to, const keeper::Answer& answer, int64_t nowMs) {
    Log("keeper: answer %u #%u %s: %s %s", to, answer.seq, answer.kind.c_str(), answer.ok ? "ok" : "no",
        answer.text.c_str());
    stats_.answers++;
    room.outbox.push_back({to, answer.seq, FormatAnswer(answer), nowMs, nowMs + kAnswerForMs});
    SendOutbox(room, nowMs);
}

void Keeper::SendOutbox(Room& room, int64_t nowMs) {
    if (!room.client) return;
    std::erase_if(room.outbox, [&](const Outgoing& o) { return nowMs >= o.untilMs; });
    for (Outgoing& o : room.outbox) {
        if (nowMs < o.nextMs) continue;
        weyve_send_p2p(room.client, o.to, o.datagram.data(), static_cast<uint32_t>(o.datagram.size()));
        if (o.sends++ > 0) stats_.resends++;
        o.nextMs = nowMs + kResendMs;
    }
}

void Keeper::OnChat(Room& room, uint32_t from, const std::string& text, int64_t nowMs) {
    auto it = room.members.find(from);
    if (it == room.members.end()) return;
    Member& m = it->second;
    if (m.removed) return;
    if (m.chat.Allow(nowMs, text)) return;
    const int strikes = m.chat.Strikes(nowMs);
    Log("keeper: room %s: %s (%u) is past the chat limits (%d in 10 s)", room.code.c_str(), m.name.c_str(), from, strikes);
    if (strikes >= ChatLimiter::kKickStrikes && room.host) {
        Log("keeper: room %s: removing %s (%u) for flooding the chat", room.code.c_str(), m.name.c_str(), from);
        m.removed = true;
        stats_.floodKicks++;
        weyve_kick_member(room.client, from);
    }
}

void Keeper::WriteData(Room& room, const std::string& key, const std::string& value) {
    if (!room.host) return;
    auto it = room.written.find(key);
    if (it != room.written.end() && it->second == value) return;
    room.written[key] = value;
    weyve_set_room_data(room.client, key.c_str(), value.c_str());
}

void Keeper::DeleteData(Room& room, const std::string& key) {
    if (!room.host || !room.written.count(key)) return;
    room.written.erase(key);
    weyve_delete_room_data(room.client, key.c_str());
}

void Keeper::UpdateCustom(Room& room, int64_t nowMs) {
    if (!room.client) return;
    if (!room.inRoom) {
        if (nowMs - room.createdMs > kOwnerWaitMs) Close(room, "never got into it");
        return;
    }
    if (!room.ownerArrived && nowMs - room.createdMs > kOwnerWaitMs) {
        Close(room, "its owner never came");
        return;
    }
    if (room.ownerArrived && room.members.empty() && room.emptySinceMs >= 0 && nowMs - room.emptySinceMs >= kEmptyCloseMs) {
        Close(room, "empty");
        return;
    }
    if (room.host && room.owner) WriteData(room, kOwner, std::to_string(room.owner));
    if (room.host) UpdateTables(room, nowMs);
    SendOutbox(room, nowMs);
}

void Keeper::UpdateLobby(Room& room, int64_t nowMs) {
    if (!room.inRoom || !room.host) return;
    if (listWrittenMs_ >= 0 && nowMs - listWrittenMs_ < kListEveryMs) return;
    listWrittenMs_ = nowMs;
    WriteListings(nowMs);
}

} // namespace sfv::keeper
