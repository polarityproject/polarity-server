#include "keeper/keeper.h"

#include "core/rules.h"

#include <weyvelength.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace sfv::keeper {

namespace {

std::string Str(const char* p, uint32_t len) { return p ? std::string(p, len) : std::string(); }

const char* RoomErrorText(WeyveRoomError e)
{
    switch (e) {
    case WEYVE_ROOM_ERROR_ALREADY_IN_ROOM: return "already in a room";
    case WEYVE_ROOM_ERROR_NO_SUCH_ROOM: return "no such room";
    case WEYVE_ROOM_ERROR_NOT_IN_ROOM: return "not in a room";
    case WEYVE_ROOM_ERROR_NOT_HOST: return "not the host";
    case WEYVE_ROOM_ERROR_BAD_ROOM_DATA: return "bad room data";
    case WEYVE_ROOM_ERROR_NO_SUCH_MEMBER: return "no such member";
    case WEYVE_ROOM_ERROR_ROOM_CLOSED: return "the room is closed";
    case WEYVE_ROOM_ERROR_BAD_PASSWORD: return "wrong password";
    case WEYVE_ROOM_ERROR_BANNED: return "banned";
    case WEYVE_ROOM_ERROR_RATE_LIMITED: return "rate limited";
    }
    return "room error";
}

constexpr int64_t kResendMs = 250;        // an answer goes again until acknowledged
constexpr int64_t kAnswerForMs = 20000;
constexpr int64_t kReconnectMs = 5000;    // the lobby's connection, after losing the server
constexpr int64_t kOwnerWaitMs = 30000;   // a new room closes if its owner never comes
constexpr int64_t kEmptyCloseMs = 3000;
constexpr int64_t kListEveryMs = 2000;    // the public list and the lobby's count, at most this often
constexpr int64_t kRoomAskEveryMs = 10000;
constexpr int64_t kPeekEveryMs = 500;
constexpr int kCallSeconds = 10;         // the your-turn card's 8 s, and a little for the trip
constexpr int kBrokeOffCallSeconds = 15; // after a set that broke off: its players are still leaving the battle
constexpr int64_t kConnectMs = 45000;    // P1 opens the match room and says its code
constexpr int64_t kAfterMs = 5000;       // the after-set panels, then the rotation

} // namespace

Keeper::Keeper(KeeperOptions options) : o_(std::move(options)) {}

Keeper::~Keeper()
{
    for (auto& r : rooms_) {
        if (r->client) weyve_client_destroy(r->client);
    }
    if (lobby_ && lobby_->client) weyve_client_destroy(lobby_->client);
}

void Keeper::Log(const char* format, ...) const
{
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (o_.log) o_.log(line);
    else std::printf("%s\n", line);
}

std::string Keeper::LobbyCode() const { return lobby_ && lobby_->inRoom ? lobby_->code : std::string(); }

size_t Keeper::CustomRooms() const { return rooms_.size(); }

bool Keeper::Connect(Room& room)
{
    room.client = weyve_client_create();
    if (!weyve_connect(room.client, o_.host.c_str(), o_.port)) {
        weyve_client_destroy(room.client);
        room.client = nullptr;
        return false;
    }
    weyve_set_name(room.client, o_.name.c_str());
    if (o_.portBegin) weyve_set_port_range(room.client, o_.portBegin, o_.portEnd);
    return true;
}

bool Keeper::Start(int64_t nowMs)
{
    now_ = nowMs;
    const std::vector<std::string> kept = LoadState();
    lobby_ = std::make_unique<Room>();
    lobby_->kind = Kind::Lobby;
    lobby_->createdMs = nowMs;
    if (!kept.empty()) lobby_->code = kept.front();
    for (size_t i = 1; i < kept.size(); i++) {
        auto r = std::make_unique<Room>();
        r->kind = Kind::Custom;
        r->code = kept[i];
        r->reclaimed = true;
        r->ownerArrived = true;
        r->createdMs = nowMs;
        rooms_.push_back(std::move(r));
    }
    if (!Connect(*lobby_)) {
        Log("keeper: cannot connect to %s:%u; trying again in %lld s", o_.host.c_str(), o_.port,
            static_cast<long long>(kReconnectMs / 1000));
        lobby_->reconnectMs = nowMs + kReconnectMs;
        return false;
    }
    Log("keeper: connected to %s:%u", o_.host.c_str(), o_.port);
    if (!lobby_->code.empty()) {
        Log("keeper: rejoining lobby channel %s", lobby_->code.c_str());
        weyve_join_room(lobby_->client, lobby_->code.c_str(), nullptr);
    }
    else {
        weyve_create_room(lobby_->client);
    }
    lobby_->joinAsked = true;
    for (auto& r : rooms_) {
        if (!Connect(*r)) {
            Close(*r, "cannot connect");
            continue;
        }
        Log("keeper: rejoining custom room %s", r->code.c_str());
        weyve_join_room(r->client, r->code.c_str(), nullptr);
        r->joinAsked = true;
    }
    return true;
}

void Keeper::Step(int64_t nowMs)
{
    now_ = nowMs;
    if (!lobby_) return;
    if (!lobby_->client && nowMs >= lobby_->reconnectMs) {
        if (Connect(*lobby_)) {
            Log("keeper: connected to %s:%u again", o_.host.c_str(), o_.port);
            lobby_->inRoom = lobby_->host = lobby_->setUp = false;
            lobby_->members.clear();
            lobby_->order.clear();
            lobby_->written.clear();
            lobby_->outbox.clear();
            if (!lobby_->code.empty()) weyve_join_room(lobby_->client, lobby_->code.c_str(), nullptr);
            else weyve_create_room(lobby_->client);
        }
        else {
            lobby_->reconnectMs = nowMs + kReconnectMs;
        }
    }
    if (lobby_->client) Pump(*lobby_, nowMs);
    for (auto& r : rooms_) {
        if (r->client && !r->closing) Pump(*r, nowMs);
    }
    for (auto& r : rooms_) {
        if (!r->closing) UpdateCustom(*r, nowMs);
    }
    const size_t before = rooms_.size();
    std::erase_if(rooms_, [](const std::unique_ptr<Room>& r) { return r->closing; });
    if (rooms_.size() != before) SaveState();
    if (lobby_->client) UpdateLobby(*lobby_, nowMs);
    SendOutbox(*lobby_, nowMs);
    Commands(nowMs);
}

void Keeper::Pump(Room& room, int64_t nowMs)
{
    WeyveClient* c = room.client;
    if (!weyve_poll(c)) {
        if (room.kind == Kind::Lobby) {
            Log("keeper: lost the server (lobby channel %s); reconnecting in %lld s", room.code.c_str(),
                static_cast<long long>(kReconnectMs / 1000));
            weyve_client_destroy(c);
            room.client = nullptr;
            room.inRoom = room.host = room.setUp = false;
            room.reconnectMs = nowMs + kReconnectMs;
        }
        else {
            Close(room, "lost the server");
        }
        return;
    }
    const uint32_t self = weyve_id(c);
    WeyveEvent e;
    while (room.client && weyve_next(c, &e)) {
        switch (e.type) {
        case WEYVE_EVENT_ROOM_ID_ASSIGNED:
            OnInRoom(room, Str(e.data.room_assigned.id, e.data.room_assigned.id_len), nowMs);
            break;
        case WEYVE_EVENT_ROOM_ERROR: {
            const WeyveRoomError code = e.data.room_error.code;
            Log("keeper: room %s: %s %s", room.code.empty() ? "(new)" : room.code.c_str(), RoomErrorText(code),
                Str(e.data.room_error.context, e.data.room_error.context_len).c_str());
            if (!room.inRoom && code == WEYVE_ROOM_ERROR_NO_SUCH_ROOM) {
                if (room.kind == Kind::Lobby) {
                    Log("keeper: lobby channel %s is gone; creating a new one", room.code.c_str());
                    room.code.clear();
                    weyve_create_room(c);
                }
                else {
                    Close(room, "gone");
                }
            }
            break;
        }
        case WEYVE_EVENT_PEER_JOINED:
            if (e.data.peer_joined.id != self)
                OnJoined(room, e.data.peer_joined.id, Str(e.data.peer_joined.name, e.data.peer_joined.name_len), nowMs);
            break;
        case WEYVE_EVENT_PEER_LEFT:
            if (e.data.peer_left.id == self) {
                room.inRoom = false;
            }
            else {
                OnLeft(room, e.data.peer_left.id, nowMs);
            }
            break;
        case WEYVE_EVENT_HOST_CHANGED: {
            const bool now = e.data.host_changed.id == self;
            if (now != room.host) Log("keeper: room %s: %s", room.code.c_str(), now ? "host again" : "host is a player now");
            room.host = now;
            if (now) SetUpAsHost(room, nowMs);
            else room.setUp = false;
            break;
        }
        case WEYVE_EVENT_MEMBER_DATA_CHANGED:
            if (e.data.member_data.id != self)
                OnMemberData(room, e.data.member_data.id, Str(e.data.member_data.key, e.data.member_data.key_len),
                             Str(e.data.member_data.value, e.data.member_data.value_len), nowMs);
            break;
        case WEYVE_EVENT_CHAT:
            if (e.data.chat.from != self) OnChat(room, e.data.chat.from, Str(e.data.chat.text, e.data.chat.text_len), nowMs);
            break;
        case WEYVE_EVENT_KICKED:
        case WEYVE_EVENT_BANNED:
            Log("keeper: removed from room %s", room.code.c_str());
            if (room.kind == Kind::Custom) Close(room, "removed");
            else room.inRoom = false;
            break;
        default:
            break;
        }
    }
    if (!room.client) return;
    // Datagrams to the keeper are not used; drained so they do not pile up.
    uint32_t from = 0, len = 0;
    while (weyve_next_p2p(c, &from, &len)) {
    }
}

void Keeper::OnInRoom(Room& room, const std::string& code, int64_t nowMs)
{
    WeyveClient* c = room.client;
    const uint32_t self = weyve_id(c);
    room.code = code;
    room.inRoom = true;
    room.host = weyve_is_host(c);
    // Proof, in our own member data: any member can check it, host or not.
    const std::string message = ProofMessage(code, self);
    uint8_t signature[64];
    if (Sign(o_.key, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature)) {
        weyve_set_member_data(c, kProof, Base64Encode(signature, sizeof(signature)).c_str());
    }
    else {
        Log("keeper: cannot sign the proof for room %s", code.c_str());
    }
    // The members already there (a rejoin after a restart).
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
    }
    else {
        if (room.reclaimed) ReadBackCustom(room);
        if (room.closing) return;
        Log("keeper: custom room %s, %s, %d tables, %s (%s)", code.c_str(), PresetName(room.settings.preset),
            room.settings.tables, room.settings.isPublic ? "public" : "private", room.host ? "host" : "waiting for host");
        if (room.requester && lobby_) {
            Answer(*lobby_, room.requester, {room.requestSeq, "room", true, code}, nowMs);
            room.requester = 0;
        }
    }
    if (room.host) SetUpAsHost(room, nowMs);
    SaveState();
}

void Keeper::ReadBackCustom(Room& room)
{
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
    room.settings.preset = std::clamp(std::atoi(data(kPreset).c_str()), 0, kPresetCount - 1);
    room.owner = static_cast<uint32_t>(std::strtoul(data(kOwner).c_str(), nullptr, 10));
    if (!room.members.count(room.owner)) {
        room.owner = NextOwner(room.order, 0, weyve_id(room.client));
    }
}

void Keeper::SetUpAsHost(Room& room, int64_t nowMs)
{
    WeyveClient* c = room.client;
    // Anything a player wrote while host is written over.
    room.written.clear();
    if (room.kind == Kind::Lobby) {
        weyve_set_room_listed(c, true);
        weyve_set_room_listing(c, kKind, kKindLobby);
        weyve_set_room_listing(c, kBuild, std::to_string(o_.protocol).c_str());
        weyve_set_room_listing(c, kCount, std::to_string(Count(room)).c_str());
        WriteData(room, kKind, kKindLobby);
        WriteData(room, kBuild, std::to_string(o_.protocol));
        listWrittenMs_ = countWrittenMs_ = -1;
    }
    else {
        weyve_set_room_listed(c, false);
        WriteData(room, kKind, kKindCustom);
        WriteData(room, kBuild, std::to_string(o_.protocol));
        WriteData(room, kPreset, std::to_string(room.settings.preset));
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
        room.masked.clear();
        for (uint32_t id : room.order) CheckName(room, id);
        WriteMask(room);
        SetUpTables(room, nowMs);
    }
    room.setUp = true;
}

void Keeper::OnJoined(Room& room, uint32_t id, const std::string& name, int64_t nowMs)
{
    if (room.members.count(id)) return; // seen already (the member list on a rejoin, then its join events)
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
            // Joined between the room filling and it closing to joiners.
            Log("keeper: room %s is full: removing %u", room.code.c_str(), id);
            weyve_kick_member(room.client, id);
        }
        else if (room.host && Count(room) >= kRoomCap) {
            weyve_set_room_joinable(room.client, false);
        }
    }
}

void Keeper::OnLeft(Room& room, uint32_t id, int64_t nowMs)
{
    const auto it = room.members.find(id);
    if (it == room.members.end()) return;
    const std::string name = it->second.name;
    room.members.erase(it);
    std::erase(room.order, id);
    std::erase_if(room.outbox, [&](const Outgoing& o) { return o.to == id; });
    if (room.kind != Kind::Custom) return;
    Log("keeper: room %s: %s (%u) left, %d members", room.code.c_str(), name.c_str(), id, Count(room));
    std::erase(room.masked, id);
    WriteMask(room); // the others' places moved
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

void Keeper::OnMemberData(Room& room, uint32_t id, const std::string& key, const std::string& value, int64_t nowMs)
{
    auto it = room.members.find(id);
    if (it == room.members.end()) return;
    Member& m = it->second;
    if (key == "ch") {
        m.chara = value.substr(0, 3);
    }
    else if (key == kAck) {
        const uint32_t seq = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        std::erase_if(room.outbox, [&](const Outgoing& o) { return o.to == id && o.seq <= seq; });
    }
    else if (key == kRequest && !value.empty()) {
        Request r;
        if (!ParseRequest(value, &r)) {
            Log("keeper: room %s: member %u sent a request that does not parse", room.code.c_str(), id);
            return;
        }
        if (r.seq <= m.lastSeq) return; // seen
        m.lastSeq = r.seq;
        OnRequest(room, m, r, nowMs);
    }
}

void Keeper::OnRequest(Room& room, Member& m, const Request& request, int64_t nowMs)
{
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
        r->owner = m.id;
        r->requester = m.id;
        r->requestSeq = request.seq;
        r->createdMs = nowMs;
        if (!Connect(*r)) {
            Answer(room, m.id, {request.seq, "room", false, "the keeper cannot reach the server"}, nowMs);
            return;
        }
        Log("keeper: %s (%u) asks for a room: %s, %d tables, %s", m.name.c_str(), m.id, PresetName(settings.preset),
            settings.tables, settings.isPublic ? "public" : "private");
        weyve_create_room(r->client);
        r->joinAsked = true;
        rooms_.push_back(std::move(r));
        return;
    }
    if (room.kind == Kind::Custom) {
        TableRequest(room, m, request, nowMs);
        return;
    }
    Answer(room, m.id, {request.seq, request.kind, false, "unknown request"}, nowMs);
}

void Keeper::Answer(Room& room, uint32_t to, const keeper::Answer& answer, int64_t nowMs)
{
    Log("keeper: answer %u #%u %s: %s %s", to, answer.seq, answer.kind.c_str(), answer.ok ? "ok" : "no",
        answer.text.c_str());
    room.outbox.push_back({to, answer.seq, FormatAnswer(answer), nowMs, nowMs + kAnswerForMs});
    SendOutbox(room, nowMs);
}

void Keeper::SendOutbox(Room& room, int64_t nowMs)
{
    if (!room.client) return;
    std::erase_if(room.outbox, [&](const Outgoing& o) { return nowMs >= o.untilMs; });
    for (Outgoing& o : room.outbox) {
        if (nowMs < o.nextMs) continue;
        weyve_send_p2p(room.client, o.to, o.datagram.data(), static_cast<uint32_t>(o.datagram.size()));
        o.nextMs = nowMs + kResendMs;
    }
}

void Keeper::OnChat(Room& room, uint32_t from, const std::string& text, int64_t nowMs)
{
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
        weyve_kick_member(room.client, from);
    }
}

void Keeper::WriteData(Room& room, const std::string& key, const std::string& value)
{
    if (!room.host) return;
    auto it = room.written.find(key);
    if (it != room.written.end() && it->second == value) return;
    room.written[key] = value;
    weyve_set_room_data(room.client, key.c_str(), value.c_str());
}

void Keeper::DeleteData(Room& room, const std::string& key)
{
    if (!room.host || !room.written.count(key)) return;
    room.written.erase(key);
    weyve_delete_room_data(room.client, key.c_str());
}

void Keeper::UpdateCustom(Room& room, int64_t nowMs)
{
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

void Keeper::UpdateLobby(Room& room, int64_t nowMs)
{
    if (!room.inRoom || !room.host) return;
    if (countWrittenMs_ < 0 || nowMs - countWrittenMs_ >= kListEveryMs) {
        countWrittenMs_ = nowMs;
        static int lastCount = -1;
        if (Count(room) != lastCount) {
            lastCount = Count(room);
            weyve_set_room_listing(room.client, kCount, std::to_string(lastCount).c_str());
            weyve_set_room_joinable(room.client, lastCount < kLobbyCap);
        }
    }
    if (listWrittenMs_ >= 0 && nowMs - listWrittenMs_ < kListEveryMs) return;
    listWrittenMs_ = nowMs;
    std::vector<ListedRoom> list;
    for (const auto& r : rooms_) {
        if (r->closing || !r->inRoom || !r->host || !r->settings.isPublic || !r->ownerArrived || r->members.empty() || r->unlisted) continue;
        ListedRoom l;
        l.code = r->code;
        l.preset = r->settings.preset;
        const auto owner = r->members.find(r->owner);
        l.owner = owner != r->members.end() ? ShownName(*r, r->owner) : std::string();
        l.members = Count(*r);
        l.tables = r->settings.tables;
        l.openSeats = 0;
        for (const Table& t : r->tables) {
            if (t.s.phase == TablePhase::Open) l.openSeats += (t.s.seat[0] ? 0 : 1) + (t.s.seat[1] ? 0 : 1);
        }
        uint32_t len = 0;
        const char* rules = weyve_room_data(r->client, TableKey(0).c_str(), &len);
        l.rules = rules ? Str(rules, len) : FormatRules(Rules{});
        for (uint32_t id : r->order) {
            const auto m = r->members.find(id);
            if (m != r->members.end() && !m->second.chara.empty() && l.faces.size() < 6) l.faces.push_back(m->second.chara);
        }
        list.push_back(std::move(l));
    }
    SortRoomList(&list);
    const std::vector<std::string> values = PackRoomList(list);
    for (int i = 0; i < kListKeys; i++) {
        if (i < static_cast<int>(values.size())) WriteData(room, ListKey(i), values[i]);
        else DeleteData(room, ListKey(i));
    }
}

void Keeper::CheckName(Room& room, uint32_t id)
{
    if (room.kind != Kind::Custom || !room.settings.isPublic || o_.words.empty()) return;
    const auto m = room.members.find(id);
    if (m == room.members.end() || !NameRejected(m->second.name, o_.words)) return;
    if (std::find(room.masked.begin(), room.masked.end(), id) != room.masked.end()) return;
    room.masked.push_back(id);
    Log("keeper: room %s: %u's name shows as %s here", room.code.c_str(), id, ShownName(room, id).c_str());
    WriteMask(room);
}

void Keeper::WriteMask(Room& room)
{
    if (room.kind != Kind::Custom) return;
    if (room.masked.empty()) {
        DeleteData(room, kMask);
        return;
    }
    std::map<uint32_t, int> numbers;
    for (uint32_t id : room.masked) {
        const auto at = std::find(room.order.begin(), room.order.end(), id);
        if (at != room.order.end()) numbers[id] = static_cast<int>(at - room.order.begin()) + 1;
    }
    WriteData(room, kMask, FormatMask(numbers));
}

std::string Keeper::ShownName(const Room& room, uint32_t id) const
{
    const auto m = room.members.find(id);
    if (m == room.members.end()) return {};
    if (std::find(room.masked.begin(), room.masked.end(), id) == room.masked.end()) return m->second.name;
    // "Player N": N its place in the room (join order), as every client draws it.
    const auto at = std::find(room.order.begin(), room.order.end(), id);
    return "Player " + std::to_string(at - room.order.begin() + 1);
}

void Keeper::Peek(Room& lobby, Member& m, const Request& request, int64_t nowMs)
{
    if (m.lastPeekMs >= 0 && nowMs - m.lastPeekMs < kPeekEveryMs) return; // asked again soon: the last answer stands
    m.lastPeekMs = nowMs;
    const std::string code = request.args.empty() ? std::string() : request.args[0];
    const Room* room = nullptr;
    for (const auto& r : rooms_) {
        if (!r->closing && r->inRoom && r->code == code && r->settings.isPublic) room = r.get();
    }
    if (!room) {
        Answer(lobby, m.id, {request.seq, "peek", false, "gone"}, nowMs);
        return;
    }
    RoomDetail d;
    for (int t = 0; t < room->settings.tables; t++) {
        uint32_t len = 0;
        const char* rules = weyve_room_data(room->client, TableKey(t).c_str(), &len);
        d.tables.push_back(rules ? Str(rules, len) : FormatRules(Rules{}));
    }
    for (uint32_t id : room->order) {
        const auto p = room->members.find(id);
        if (p != room->members.end()) d.people.push_back({ShownName(*room, id), p->second.chara});
    }
    for (const std::string& part : PackRoomDetail(d)) {
        lobby.outbox.push_back({m.id, request.seq, FormatAnswer({request.seq, "peek", true, part}), nowMs, nowMs + 5000});
    }
    SendOutbox(lobby, nowMs);
}

// ---- tables ---------------------------------------------------------------------

void Keeper::SetUpTables(Room& room, int64_t nowMs)
{
    // Kept as they were (a keeper that restarts reads them back); new
    // tables open and empty.
    const int count = room.settings.tables;
    room.tables.resize(count);
    for (int t = 0; t < count; t++) {
        Table& table = room.tables[t];
        uint32_t len = 0;
        const char* rules = weyve_room_data(room.client, TableKey(t).c_str(), &len);
        if (!rules || !ParseRules(Str(rules, len), &table.rules)) table.rules = Rules{};
        const char* state = weyve_room_data(room.client, StateKey(t).c_str(), &len);
        if (!state || !ParseTable(Str(state, len), &table.s)) table.s = TableState{};
        const char* queue = weyve_room_data(room.client, QueueKey(t).c_str(), &len);
        table.queue = queue ? ParseIds(Str(queue, len)) : std::vector<uint32_t>{};
        // Only members still here; a set that was running when the keeper
        // went goes on without it (its players report the result).
        std::erase_if(table.queue, [&](uint32_t id) { return !room.members.count(id); });
        for (uint32_t& id : table.s.seat) {
            if (id && !room.members.count(id)) id = 0;
        }
        table.setRules = table.rules;
        table.phaseAtMs = nowMs;
        table.written.clear();
        if (table.s.phase == TablePhase::Call || table.s.phase == TablePhase::Connecting || table.s.phase == TablePhase::After) {
            Fill(room, table, nowMs, table.s.phase == TablePhase::Connecting); // whatever was in progress starts over
        }
    }
    WriteTables(room);
}

void Keeper::SetPhase(Table& t, TablePhase phase, int64_t nowMs)
{
    t.s.phase = phase;
    t.phaseAtMs = nowMs;
}

void Keeper::Fill(Room& room, Table& t, int64_t nowMs, bool brokeOff)
{
    FillSeats(&t.s, &t.queue);
    t.phaseAtMs = nowMs;
    if (t.s.phase == TablePhase::Call) {
        t.s.call++;
        t.s.callSeconds = static_cast<uint8_t>(brokeOff ? kBrokeOffCallSeconds : kCallSeconds);
        t.setRules = t.rules;
        const int index = static_cast<int>(&t - room.tables.data());
        Log("keeper: room %s: table %d calls %u and %u", room.code.c_str(), index + 1, t.s.seat[0], t.s.seat[1]);
    }
}

void Keeper::WriteTables(Room& room)
{
    if (!room.host) return;
    for (int i = 0; i < static_cast<int>(room.tables.size()); i++) {
        Table& t = room.tables[i];
        const std::string state = FormatTable(t.s), queue = FormatIds(t.queue);
        if (state + "\n" + queue == t.written) continue;
        t.written = state + "\n" + queue;
        WriteData(room, StateKey(i), state);
        if (queue.empty()) DeleteData(room, QueueKey(i));
        else WriteData(room, QueueKey(i), queue);
    }
}

void Keeper::Ticket(Room& room, uint32_t to, const TableTicket& ticket, int64_t nowMs)
{
    // Sent again until acknowledged, as an answer is; numbered after the
    // player's last request, so their acknowledgement covers it.
    const auto m = room.members.find(to);
    const uint32_t seq = m != room.members.end() ? m->second.lastSeq + 1000000 : 1;
    Log("keeper: room %s: ticket to %u: %s", room.code.c_str(), to, FormatTicket(ticket).c_str());
    room.outbox.push_back({to, seq, FormatAnswer({seq, "ticket", true, FormatTicket(ticket)}), nowMs, nowMs + kConnectMs});
    SendOutbox(room, nowMs);
}

void Keeper::OffTables(Room& room, uint32_t id, int64_t nowMs)
{
    for (Table& t : room.tables) {
        const bool seated = t.s.seat[0] == id || t.s.seat[1] == id;
        const bool inSet = t.s.phase == TablePhase::Connecting || t.s.phase == TablePhase::Playing;
        if (!LeaveTable(id, &t.s, &t.queue)) continue;
        if (seated && t.s.phase != TablePhase::Open) {
            // A call or a set without one of its players: the other keeps
            // the seat, the next in line takes the empty one.
            Fill(room, t, nowMs, inSet);
        }
        else if (t.s.phase == TablePhase::Open) {
            Fill(room, t, nowMs);
        }
    }
    WriteTables(room);
}

void Keeper::TableRequest(Room& room, Member& m, const Request& r, int64_t nowMs)
{
    if (!room.host) return; // paused: nothing moves
    const int count = static_cast<int>(room.tables.size());
    int index = -1;
    if (!r.args.empty()) index = std::atoi(r.args[0].c_str());
    Table* t = index >= 0 && index < count ? &room.tables[index] : nullptr;
    auto seatOf = [&](const Table& table) { return table.s.seat[0] == m.id ? 0 : table.s.seat[1] == m.id ? 1 : -1; };
    if (r.kind == "seat" && t && r.args.size() == 2) {
        // Off any other table first (one place at a time).
        for (Table& other : room.tables) {
            if (&other != t && LeaveTable(m.id, &other.s, &other.queue) && other.s.phase != TablePhase::Open) Fill(room, other, nowMs);
        }
        const int want = std::atoi(r.args[1].c_str());
        if (t->s.phase == TablePhase::Open && seatOf(*t) < 0) {
            int seat = want == 0 || want == 1 ? want : (!t->s.seat[0] ? 0 : 1);
            if (t->s.seat[seat]) seat = 1 - seat;
            if (!t->s.seat[seat]) {
                std::erase(t->queue, m.id);
                t->s.seat[seat] = m.id;
                Log("keeper: room %s: %s sits at table %d as P%d", room.code.c_str(), m.name.c_str(), index + 1, seat + 1);
                if (t->s.seat[0] && t->s.seat[1]) Fill(room, *t, nowMs);
            }
        }
    }
    else if (r.kind == "stand") {
        for (Table& table : room.tables) {
            if (seatOf(table) < 0) continue;
            if (table.s.phase == TablePhase::Open || table.s.phase == TablePhase::Call) {
                LeaveTable(m.id, &table.s, &table.queue);
                Fill(room, table, nowMs);
            }
            else if (table.s.phase == TablePhase::Connecting || table.s.phase == TablePhase::Playing) {
                // Cancel on the connecting modal (clients send it only before
                // their match began): as if they had left the room, the other
                // keeps the seat and the table is called again.
                LeaveTable(m.id, &table.s, &table.queue);
                Log("keeper: room %s: %s leaves table %d before its match", room.code.c_str(), m.name.c_str(),
                    static_cast<int>(&table - room.tables.data()) + 1);
                Fill(room, table, nowMs, true);
            }
        }
    }
    else if (r.kind == "queue" && t) {
        for (Table& other : room.tables) {
            if (&other != t && LeaveTable(m.id, &other.s, &other.queue) && other.s.phase != TablePhase::Open) Fill(room, other, nowMs);
        }
        if (seatOf(*t) < 0 && std::find(t->queue.begin(), t->queue.end(), m.id) == t->queue.end()) {
            t->queue.push_back(m.id);
            m.callStrikes = 0;
            Log("keeper: room %s: %s queues for table %d (%zu in queue)", room.code.c_str(), m.name.c_str(), index + 1, t->queue.size());
            // A table nobody waits at takes them straight into a seat.
            if (t->s.phase == TablePhase::Open) Fill(room, *t, nowMs);
        }
    }
    else if (r.kind == "unqueue") {
        for (Table& table : room.tables) std::erase(table.queue, m.id);
    }
    else if (r.kind == "ready" && t && t->s.phase == TablePhase::Call) {
        const int seat = seatOf(*t);
        if (seat >= 0) {
            t->s.ready[seat] = true;
            m.callStrikes = 0;
            if (t->s.ready[0] && t->s.ready[1]) {
                // P1 hosts the match room; P2's ticket goes once it is open.
                t->token = rng_() | 1;
                SetPhase(*t, TablePhase::Connecting, nowMs);
                Ticket(room, t->s.seat[0], {true, index, "", t->token, FormatRules(t->setRules)}, nowMs);
            }
        }
    }
    else if (r.kind == "decline" && t && seatOf(*t) >= 0 &&
             (t->s.phase == TablePhase::Call || t->s.phase == TablePhase::Open)) {
        LeaveTable(m.id, &t->s, &t->queue);
        Log("keeper: room %s: %s leaves table %d's queue", room.code.c_str(), m.name.c_str(), index + 1);
        Fill(room, *t, nowMs);
    }
    else if (r.kind == "code" && t && r.args.size() == 2 && t->s.phase == TablePhase::Connecting && seatOf(*t) == 0) {
        t->s.code = r.args[1];
        SetPhase(*t, TablePhase::Playing, nowMs);
        Ticket(room, t->s.seat[1], {false, index, t->s.code, t->token, FormatRules(t->setRules)}, nowMs);
    }
    else if ((r.kind == "score" || r.kind == "result") && t && r.args.size() == 3 &&
             (t->s.phase == TablePhase::Playing || t->s.phase == TablePhase::Connecting)) {
        // P1 reports; P2 only once P1 is gone.
        const int seat = seatOf(*t);
        if (seat == 0 || (seat == 1 && !room.members.count(t->s.seat[0]))) {
            const uint8_t games[2] = {static_cast<uint8_t>(std::clamp(std::atoi(r.args[1].c_str()), 0, 99)),
                                      static_cast<uint8_t>(std::clamp(std::atoi(r.args[2].c_str()), 0, 99))};
            if (r.kind == "score") {
                t->s.wins[0] = games[0];
                t->s.wins[1] = games[1];
            }
            else if (games[0] == games[1]) {
                Log("keeper: room %s: table %d's set did not finish; calling it again", room.code.c_str(), index + 1);
                Fill(room, *t, nowMs, true);
            }
            else {
                ApplySetResult(t->setRules, games[0] > games[1] ? 0 : 1, games, &t->s, &t->queue);
                t->phaseAtMs = nowMs;
                Log("keeper: room %s: table %d: %u beat %u %d-%d", room.code.c_str(), index + 1, t->s.lastWinner, t->s.lastLoser,
                    t->s.lastScore[0], t->s.lastScore[1]);
            }
        }
    }
    else if (r.kind == "rules" && t && r.args.size() == 2 && m.id == room.owner) {
        Rules rules;
        if (ParseRules(r.args[1], &rules)) {
            t->rules = rules;
            WriteData(room, TableKey(index), FormatRules(rules));
            Log("keeper: room %s: table %d's rules are %s from its next set", room.code.c_str(), index + 1, r.args[1].c_str());
        }
        else {
            Log("keeper: room %s: table %d: rules '%s' do not parse; kept", room.code.c_str(), index + 1, r.args[1].c_str());
        }
    }
    else if (r.kind == "kick" && !r.args.empty() && m.id == room.owner) {
        const uint32_t id = static_cast<uint32_t>(std::strtoul(r.args[0].c_str(), nullptr, 10));
        if (id && id != m.id && room.members.count(id)) {
            Log("keeper: room %s: the owner removes %s (%u)", room.code.c_str(), room.members[id].name.c_str(), id);
            // Barred until they reconnect (Weyvelength's ban holds the connection).
            weyve_ban_member(room.client, id);
        }
    }
    WriteTables(room);
}

void Keeper::UpdateTables(Room& room, int64_t nowMs)
{
    for (int i = 0; i < static_cast<int>(room.tables.size()); i++) {
        Table& t = room.tables[i];
        const int64_t in = nowMs - t.phaseAtMs;
        if (t.s.phase == TablePhase::Call && in >= 1000 * (t.s.callSeconds ? t.s.callSeconds : kCallSeconds)) {
            // Who did not ready goes to the back of the queue; a second
            // lapse in a row takes them off it. Who did keeps the seat and
            // their Ready for the next call.
            const uint32_t wasReady[2] = {t.s.ready[0] ? t.s.seat[0] : 0, t.s.ready[1] ? t.s.seat[1] : 0};
            for (int seat = 0; seat < 2; seat++) {
                const uint32_t id = t.s.seat[seat];
                if (!id || t.s.ready[seat]) continue;
                t.s.seat[seat] = 0;
                auto m = room.members.find(id);
                const int strikes = m != room.members.end() ? ++m->second.callStrikes : 2;
                if (strikes < 2) t.queue.push_back(id);
                Log("keeper: room %s: table %d: %u did not answer the call (%s)", room.code.c_str(), i + 1, id,
                    strikes < 2 ? "back of the queue" : "off the queue");
            }
            Fill(room, t, nowMs);
            for (int seat = 0; seat < 2; seat++) {
                if (t.s.phase == TablePhase::Call && wasReady[seat] && t.s.seat[seat] == wasReady[seat]) t.s.ready[seat] = true;
            }
        }
        else if (t.s.phase == TablePhase::Connecting && in >= kConnectMs) {
            Log("keeper: room %s: table %d's match room never opened; calling again", room.code.c_str(), i + 1);
            Fill(room, t, nowMs, true);
        }
        else if (t.s.phase == TablePhase::After && in >= kAfterMs) {
            Fill(room, t, nowMs);
        }
    }
    WriteTables(room);
}

void Keeper::Commands(int64_t nowMs)
{
    if (o_.statePath.empty() || nowMs - commandsAtMs_ < 1000) return;
    commandsAtMs_ = nowMs;
    const std::string path = o_.statePath + ".cmd";
    std::ifstream in(path);
    if (!in) return;
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    in.close();
    std::remove(path.c_str());
    for (const std::string& line : lines) {
        std::istringstream ss(line);
        std::string cmd, arg;
        ss >> cmd >> arg;
        if (cmd == "unlist" || cmd == "list") {
            bool found = false;
            for (auto& r : rooms_) {
                if (r->code != arg) continue;
                r->unlisted = cmd == "unlist";
                found = true;
            }
            listWrittenMs_ = -1; // the list again at once
            Log("keeper: operator: %s %s%s", cmd.c_str(), arg.c_str(), found ? "" : " (no such room)");
        }
        else if (cmd == "word" && !arg.empty()) {
            const std::string w = FoldName(arg);
            if (w.size() >= 3) {
                o_.words.push_back(w);
                Log("keeper: operator: '%s' added to the name filter (%zu words)", w.c_str(), o_.words.size());
                for (auto& r : rooms_) {
                    for (uint32_t id : r->order) CheckName(*r, id);
                }
            }
        }
        else if (cmd == "rooms") {
            Log("keeper: operator: lobby %s, %zu custom rooms", lobby_ ? lobby_->code.c_str() : "-", rooms_.size());
            for (const auto& r : rooms_) {
                Log("keeper:   %s %s %s, %d members, %d tables%s%s", r->code.c_str(), PresetName(r->settings.preset),
                    r->settings.isPublic ? "public" : "private", Count(*r), r->settings.tables, r->unlisted ? ", unlisted" : "",
                    r->host ? "" : ", paused");
            }
        }
        else {
            Log("keeper: operator: unknown command '%s'", line.c_str());
        }
    }
}

void Keeper::Close(Room& room, const char* why)
{
    if (room.closing) return;
    Log("keeper: closing room %s: %s", room.code.empty() ? "(new)" : room.code.c_str(), why);
    room.closing = true;
    if (room.requester && lobby_) {
        // Asked for and never made.
        Answer(*lobby_, room.requester, {room.requestSeq, "room", false, "the room could not be made"}, now_);
        room.requester = 0;
    }
    if (room.client) {
        if (room.inRoom) weyve_leave_room(room.client);
        for (int i = 0; i < 3 && weyve_poll(room.client); i++) {
        }
        weyve_client_destroy(room.client);
        room.client = nullptr;
    }
}

std::vector<std::string> Keeper::LoadState()
{
    std::vector<std::string> codes;
    if (o_.statePath.empty()) return codes;
    std::ifstream in(o_.statePath);
    std::string kind, code;
    std::string lobby;
    std::vector<std::string> custom;
    while (in >> kind >> code) {
        if (kind == "lobby") lobby = code;
        else if (kind == "room") custom.push_back(code);
    }
    codes.push_back(lobby);
    codes.insert(codes.end(), custom.begin(), custom.end());
    if (lobby.empty() && custom.empty()) codes.clear();
    else Log("keeper: state %s: lobby %s, %zu custom rooms", o_.statePath.c_str(), lobby.empty() ? "-" : lobby.c_str(),
             custom.size());
    return codes;
}

void Keeper::SaveState()
{
    if (o_.statePath.empty() || !lobby_) return;
    std::ostringstream s;
    if (!lobby_->code.empty()) s << "lobby " << lobby_->code << "\n";
    for (const auto& r : rooms_) {
        if (!r->closing && !r->code.empty()) s << "room " << r->code << "\n";
    }
    if (s.str() == stateWritten_) return;
    const std::string temp = o_.statePath + ".new";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << s.str();
        if (!out) {
            Log("keeper: cannot write %s", temp.c_str());
            return;
        }
    }
    std::remove(o_.statePath.c_str());
    if (std::rename(temp.c_str(), o_.statePath.c_str()) == 0) stateWritten_ = s.str();
}

} // namespace sfv::keeper
