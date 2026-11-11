#include "keeper/keeper.h"

#include "core/lobby.h"
#include "core/rules.h"

#include <weyvelength.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sfv::keeper {

namespace {

std::string Str(const char* p, uint32_t len) { return p ? std::string(p, len) : std::string(); }

// A session message on the netplay driver's control channel (its Mux tag).
constexpr uint8_t kControlChannel = 1;
void SendLobby(WeyveClient* c, uint64_t to, const lobby::Message& m)
{
    if (!c) return;
    const std::vector<uint8_t> bytes = lobby::Encode(m);
    std::vector<uint8_t> tagged(bytes.size() + 1);
    tagged[0] = kControlChannel;
    std::copy(bytes.begin(), bytes.end(), tagged.begin() + 1);
    weyve_send_p2p(c, static_cast<uint32_t>(to), tagged.data(), static_cast<uint32_t>(tagged.size()));
}

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
// The your-turn card shows a call's time less 1.5 s (the trip): 20 s to ready
// (user, 2026-11-07: a player called while watching another table leaves
// that match first, and the call's time runs meanwhile).
constexpr int kCallSeconds = 21;
constexpr int kBrokeOffCallSeconds = 26; // after a set that broke off: its players are still leaving the battle
constexpr int64_t kConnectMs = 45000;    // P1 opens the match room and says its code
constexpr int64_t kAfterMs = 5000;       // the after-set panels, then the rotation
constexpr int64_t kConnectWaitMs = 30000; // a connect not over by then counts as failed
constexpr int kConnectThreads = 4;

} // namespace

Keeper::Keeper(KeeperOptions options) : o_(std::move(options))
{
    listRev_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    connector_ = std::make_unique<Connector<WeyveClient>>(
        [this] { return NewClient(); },
        [](WeyveClient* c) {
            weyve_leave_room(c);
            for (int i = 0; i < 3 && weyve_poll(c); i++) {
            }
            weyve_client_destroy(c);
        },
        kConnectThreads);
}

Keeper::~Keeper()
{
    for (auto& r : rooms_) {
        for (auto& relay : r->relays) {
            if (relay->up) weyve_client_destroy(relay->up);
            if (relay->down) weyve_client_destroy(relay->down);
        }
        if (r->client) weyve_client_destroy(r->client);
    }
    if (lobby_ && lobby_->client) weyve_client_destroy(lobby_->client);
    connector_.reset(); // its workers stop; what they held is destroyed
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

WeyveClient* Keeper::NewClient() const
{
    // On a Connector worker: a connect that waits (a firewall dropping new
    // connections, TCP retrying, a slow name lookup) holds up no room.
    WeyveClient* c = weyve_client_create();
    if (!weyve_connect(c, o_.host.c_str(), o_.port)) {
        weyve_client_destroy(c);
        return nullptr;
    }
    weyve_set_name(c, o_.name.c_str());
    if (o_.portBegin) weyve_set_port_range(c, o_.portBegin, o_.portEnd);
    weyve_set_link_mux(c, o_.linkMux);
    return c;
}

void Keeper::Ask(uint64_t& ticket, int64_t& askedMs, int64_t nowMs)
{
    if (ticket) return;
    ticket = connector_->Request();
    askedMs = nowMs;
}

bool Keeper::Arrived(uint64_t& ticket, int64_t askedMs, int64_t nowMs, WeyveClient** client)
{
    if (!ticket) return false;
    double took = 0;
    if (!connector_->Take(ticket, client, &took)) {
        if (nowMs - askedMs < kConnectWaitMs) return false;
        // Still not through: given up (its client, if it ever comes, is destroyed).
        connector_->Cancel(ticket);
        *client = nullptr;
        took = (nowMs - askedMs) / 1000.0;
    }
    ticket = 0;
    stats_.connects++;
    stats_.connectSeconds += took;
    stats_.connectMaxSeconds = std::max(stats_.connectMaxSeconds, took);
    if (!*client) stats_.connectFailures++;
    return true;
}

void Keeper::Retire(WeyveClient*& client)
{
    connector_->Retire(client);
    client = nullptr;
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
    // Connected by Step as the connects come through.
    Log("keeper: connecting to %s:%u", o_.host.c_str(), o_.port);
    Ask(lobby_->connecting, lobby_->connectAskedMs, nowMs);
    for (auto& r : rooms_) Ask(r->connecting, r->connectAskedMs, nowMs);
    return true;
}

void Keeper::ConnectedLobby(int64_t nowMs)
{
    (void)nowMs;
    Log("keeper: connected to %s:%u%s", o_.host.c_str(), o_.port, everConnected_ ? " again" : "");
    everConnected_ = true;
    lobby_->inRoom = lobby_->host = lobby_->setUp = false;
    lobby_->members.clear();
    lobby_->order.clear();
    lobby_->written.clear();
    lobby_->outbox.clear();
    if (!lobby_->code.empty()) {
        Log("keeper: rejoining lobby channel %s", lobby_->code.c_str());
        weyve_join_room(lobby_->client, lobby_->code.c_str(), nullptr);
    }
    else {
        FindLobby(*lobby_);
    }
    lobby_->joinAsked = true;
}

void Keeper::ConnectedRoom(Room& room, int64_t nowMs)
{
    (void)nowMs;
    if (room.reclaimed) {
        // Kept from before a restart.
        Log("keeper: rejoining custom room %s", room.code.c_str());
        weyve_join_room(room.client, room.code.c_str(), nullptr);
    }
    else {
        weyve_create_room(room.client);
    }
    room.joinAsked = true;
}

void Keeper::Step(int64_t nowMs)
{
    now_ = nowMs;
    if (!lobby_) return;
    const auto stepStart = std::chrono::steady_clock::now();
    // Connects that came through (or failed) since the last step.
    if (lobby_->connecting) {
        WeyveClient* c = nullptr;
        if (Arrived(lobby_->connecting, lobby_->connectAskedMs, nowMs, &c)) {
            if (c) {
                lobby_->client = c;
                ConnectedLobby(nowMs);
            }
            else {
                Log("keeper: cannot connect to %s:%u; trying again in %lld s", o_.host.c_str(), o_.port,
                    static_cast<long long>(kReconnectMs / 1000));
                lobby_->reconnectMs = nowMs + kReconnectMs;
            }
        }
    }
    else if (!lobby_->client && nowMs >= lobby_->reconnectMs) {
        Ask(lobby_->connecting, lobby_->connectAskedMs, nowMs);
    }
    for (auto& r : rooms_) {
        if (!r->connecting || r->closing) continue;
        WeyveClient* c = nullptr;
        if (!Arrived(r->connecting, r->connectAskedMs, nowMs, &c)) continue;
        if (!c) {
            Close(*r, "cannot connect");
            continue;
        }
        r->client = c;
        ConnectedRoom(*r, nowMs);
    }
    if (lobby_->client) Pump(*lobby_, nowMs);
    for (auto& r : rooms_) {
        if (r->client && !r->closing) Pump(*r, nowMs);
    }
    for (auto& r : rooms_) {
        if (!r->closing) PumpRelays(*r, nowMs);
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
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - stepStart).count();
    stats_.steps++;
    stats_.stepSeconds += took;
    stats_.stepMaxSeconds = std::max(stats_.stepMaxSeconds, took);
    for (int i = 0; i < Stats::kBuckets; i++) {
        if (took <= Stats::kBucketSeconds[i]) {
            stats_.stepBuckets[i]++;
            break;
        }
    }
}

void Keeper::Pump(Room& room, int64_t nowMs)
{
    WeyveClient* c = room.client;
    if (!weyve_poll(c)) {
        if (room.kind == Kind::Lobby) {
            stats_.serverLost++;
            Log("keeper: lost the server (lobby channel %s); reconnecting in %lld s", room.code.c_str(),
                static_cast<long long>(kReconnectMs / 1000));
            Retire(room.client);
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
                    Log("keeper: lobby channel %s is gone; looking for another", room.code.c_str());
                    room.code.clear();
                    FindLobby(room);
                }
                else {
                    Close(room, "gone");
                }
            }
            break;
        }
        case WEYVE_EVENT_ROOM_LIST:
            if (room.kind == Kind::Lobby && room.lobbyListAsked) OnLobbyList(room);
            break;
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

// No lobby channel of its own (first start, a lost state file, or the saved
// one gone): the players may already sit in one of this build's, made by an
// earlier keeper (another state, a container recreated without its volume).
// A second channel split them: the hubs pick the busiest, so they stayed in
// the old one with no keeper ("custom rooms are not available", 2026-11-05).
// That one is joined; the players hand its host back to the keeper.
void Keeper::FindLobby(Room& room)
{
    const std::string build = std::to_string(o_.protocol);
    const WeyveRoomFilter filters[] = {{kKind, WEYVE_FILTER_EQUALS, kKindLobby}, {kBuild, WEYVE_FILTER_EQUALS, build.c_str()}};
    room.lobbyListAsked = weyve_list_rooms(room.client, filters, 2);
    if (!room.lobbyListAsked) weyve_create_room(room.client);
}

void Keeper::OnLobbyList(Room& room)
{
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
    room.version = data(kVersion).substr(0, kVersionMax);
    // Its title as written (an older or broken one: none).
    if (!ParseTitle(data(kTitle), &room.settings.title)) room.settings.title = RoomTitle{};
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
    room.listed.clear();
    room.listedOn = false;
    if (room.kind == Kind::Lobby) {
        // The desk: its kind here, its count and summary (signed) from
        // WriteListings.
        weyve_set_room_listed(c, true);
        weyve_set_room_listing(c, kKind, kKindLobby);
        room.listed[kKind] = kKindLobby;
        WriteData(room, kKind, kKindLobby);
        WriteData(room, kBuild, std::to_string(o_.protocol));
        listWrittenMs_ = -1;
    }
    else {
        // Listed by WriteListings once it is a public room with its owner in.
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
    room.maskNumbers.erase(id);
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
    if (it == room.members.end()) {
        if (key == kRequest && !value.empty() && room.kind == Kind::Custom)
            Log("keeper: room %s: a request from %u, who has not joined here yet: %s", room.code.c_str(), id, value.c_str());
        return;
    }
    Member& m = it->second;
    if (key == "ch") {
        m.chara = value.substr(0, 3);
    }
    else if (key == "@cc") {
        // The rooms server's own: the member's country from their address
        // (polarity-server docker/weyvelength-geoip.patch; clients cannot set '@'
        // keys). Two letters (ISO 3166 alpha-2) or nothing: it goes out on the
        // rooms API.
        const bool ok = value.size() == 2 && std::isupper(static_cast<unsigned char>(value[0])) &&
                        std::isupper(static_cast<unsigned char>(value[1]));
        m.country = ok ? value : std::string();
    }
    else if (key == kVersion) {
        m.version = value.substr(0, kVersionMax);
        if (room.kind == Kind::Custom && !room.version.empty() && !m.version.empty() && m.version != room.version)
            Log("keeper: room %s: %s (%u) runs Polarity %s, the room %s", room.code.c_str(), m.name.c_str(), id, m.version.c_str(),
                room.version.c_str());
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
    {
        // Known kinds only: what members send is theirs (no unbounded label values).
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
        // Made once its connect comes through (Step); a failed one answers no (Close).
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

void Keeper::Answer(Room& room, uint32_t to, const keeper::Answer& answer, int64_t nowMs)
{
    Log("keeper: answer %u #%u %s: %s %s", to, answer.seq, answer.kind.c_str(), answer.ok ? "ok" : "no",
        answer.text.c_str());
    stats_.answers++;
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
        if (o.sends++ > 0) stats_.resends++;
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
        stats_.floodKicks++;
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
    if (listWrittenMs_ >= 0 && nowMs - listWrittenMs_ < kListEveryMs) return;
    listWrittenMs_ = nowMs;
    WriteListings(nowMs);
}

std::string Keeper::SignText(const std::string& message) const
{
    uint8_t signature[64];
    if (!Sign(o_.key, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature)) return {};
    return Base64Encode(signature, sizeof(signature));
}

// The public list in the server's own room listing (docs/room-listing-plan.md):
// each public room's slots, signed, set by the keeper as its host, and the
// desk's count and summary. Players list them without joining a room. At most
// every kListEveryMs (UpdateLobby).
void Keeper::WriteListings(int64_t nowMs)
{
    (void)nowMs;
    auto set = [](WeyveClient* c, Listing& had, const Listing& want) {
        for (const auto& [key, value] : want) {
            const auto it = had.find(key);
            if (it == had.end() || it->second != value) weyve_set_room_listing(c, key.c_str(), value.c_str());
        }
        for (const auto& [key, value] : had) {
            if (!want.count(key)) weyve_set_room_listing(c, key.c_str(), "");
        }
        had = want;
    };
    const std::vector<ListedRoom> list = PublicList();
    std::map<std::string, const ListedRoom*> byCode;
    for (const ListedRoom& l : list) byCode[l.code] = &l;
    for (auto& r : rooms_) {
        if (r->closing || !r->client || !r->inRoom || !r->host) continue;
        const auto it = byCode.find(r->code);
        if (it == byCode.end()) {
            // Private, the owner not in yet, empty, or taken off the list.
            if (r->listedOn) {
                weyve_set_room_listed(r->client, false);
                r->listedOn = false;
                listRev_++;
            }
            continue;
        }
        Listing want = RoomListing(*it->second, o_.protocol);
        Listing had = r->listed;
        had.erase(kListingSig);
        if (r->listedOn && had == want) continue;
        const std::string sig = SignText(ListingMessage(r->code, want));
        if (sig.empty()) {
            Log("keeper: room %s: cannot sign its listing", r->code.c_str());
            continue;
        }
        want[kListingSig] = sig;
        set(r->client, r->listed, want);
        if (!r->listedOn) weyve_set_room_listed(r->client, true);
        r->listedOn = true;
        listRev_++;
    }
    // The desk: who is in it, and the summary players poll.
    Room& desk = *lobby_;
    if (!desk.client || !desk.inRoom || !desk.host) return;
    DeskSummary summary;
    summary.rev = listRev_;
    summary.publicRooms = static_cast<int>(list.size());
    for (const ListedRoom& l : list) {
        summary.players += l.members;
        if (l.version.empty()) continue;
        auto v = std::find_if(summary.versions.begin(), summary.versions.end(), [&](const DeskSummary::Version& x) { return x.version == l.version; });
        if (v == summary.versions.end()) v = summary.versions.insert(summary.versions.end(), {l.version, 0, 0});
        v->publicRooms++;
        v->players += l.members;
    }
    // The busiest versions first (the value is cut at 128 bytes).
    std::stable_sort(summary.versions.begin(), summary.versions.end(),
                     [](const DeskSummary::Version& a, const DeskSummary::Version& b) { return a.players > b.players; });
    Listing want{{kKind, kKindLobby},
                 {kBuild, std::to_string(o_.protocol)},
                 {kCount, std::to_string(Count(desk))},
                 {kDeskSummary, FormatDeskSummary(summary)}};
    Listing had = desk.listed;
    had.erase(kListingSig);
    if (had == want) return;
    const std::string sig = SignText(DeskMessage(desk.code, want));
    if (sig.empty()) return;
    want[kListingSig] = sig;
    set(desk.client, desk.listed, want);
    weyve_set_room_joinable(desk.client, Count(desk) < kLobbyCap);
}

std::vector<ListedRoom> Keeper::PublicList() const
{
    std::vector<ListedRoom> list;
    for (const auto& r : rooms_) {
        if (r->closing || !r->inRoom || !r->host || !r->settings.isPublic || !r->ownerArrived || r->members.empty() || r->unlisted) continue;
        ListedRoom l;
        l.code = r->code;
        l.title = r->settings.title;
        l.version = r->version;
        const auto owner = r->members.find(r->owner);
        l.owner = owner != r->members.end() ? ShownName(*r, r->owner) : std::string();
        l.ownerCountry = owner != r->members.end() ? owner->second.country : std::string();
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
            if (m == r->members.end()) continue;
            if (!m->second.chara.empty() && l.faces.size() < 6) l.faces.push_back(m->second.chara);
            l.players.push_back({ShownName(*r, id), m->second.country, m->second.chara});
        }
        list.push_back(std::move(l));
    }
    SortRoomList(&list);
    return list;
}

Keeper::ServerStats Keeper::ReadServerStats() const
{
    ServerStats stats;
    if (o_.serverStatsPath.empty()) return stats;
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(o_.serverStatsPath, ec);
    if (ec) return stats;
    stats.ageSeconds = std::max(0.0, std::chrono::duration<double>(std::filesystem::file_time_type::clock::now() - written).count());
    std::ifstream in(o_.serverStatsPath);
    std::stringstream text;
    text << in.rdbuf();
    const std::string s = text.str();
    const size_t at = s.find("\"connections\":");
    if (at != std::string::npos) stats.connections = std::strtol(s.c_str() + at + 14, nullptr, 10);
    return stats;
}

int Keeper::OnlineFromServer(const ServerStats& stats) const
{
    if (!stats.Fresh()) return -1;
    const long connections = stats.connections;
    // Connections that are not one player each: the keeper's own (a room,
    // relay rooms), and the second ones players open for a match (both
    // players of a table set) or to watch one through a relay.
    long others = 0;
    if (lobby_ && lobby_->client) others++;
    for (const auto& r : rooms_) {
        if (r->client) others++;
        for (const auto& relay : r->relays) {
            others += (relay->up ? 1 : 0) + (relay->down ? 1 : 0);
            if (relay->core && !relay->ended) others += static_cast<long>(relay->core->Stats().viewers);
        }
        for (const Table& t : r->tables) {
            if (t.s.phase == TablePhase::Connecting || t.s.phase == TablePhase::Playing) others += 2;
        }
    }
    return static_cast<int>(std::max(0L, connections - others));
}

int Keeper::PlayersOnline(bool* fromServer) const
{
    // From the server when it says (browsing players are in no room); else a
    // player is in one room at a time (the desk or a custom room), so the
    // members add up without counting anyone twice.
    const int online = OnlineFromServer(ReadServerStats());
    *fromServer = online >= 0;
    if (online >= 0) return online;
    int members = 0;
    if (lobby_ && lobby_->inRoom) members = Count(*lobby_);
    for (const auto& r : rooms_) {
        if (!r->closing && r->inRoom) members += Count(*r);
    }
    return members;
}

std::string Keeper::StatusJson(int64_t unixMs) const
{
    RoomTotals totals;
    bool fromServer = false;
    totals.online = PlayersOnline(&fromServer);
    const std::vector<ListedRoom> list = PublicList();
    totals.publicRooms = static_cast<int>(list.size());
    for (const ListedRoom& l : list) totals.inPublicRooms += l.members;
    return FormatRoomListJson(list, totals, unixMs);
}

std::string Keeper::MetricsText()
{
    std::string out;
    auto line = [&](const std::string& name, const std::string& labels, double value) {
        char number[64];
        std::snprintf(number, sizeof(number), "%.9g", value);
        out += name + (labels.empty() ? "" : "{" + labels + "}") + " " + number + "\n";
    };
    auto head = [&](const char* name, const char* type, const char* help) {
        out += std::string("# HELP ") + name + " " + help + "\n# TYPE " + name + " " + type + "\n";
    };
    head("sfvr_keeper_steps_total", "counter", "Main loop steps.");
    line("sfvr_keeper_steps_total", "", static_cast<double>(stats_.steps));
    head("sfvr_keeper_step_seconds", "histogram", "Time one main loop step took (every client polled, every room updated).");
    uint64_t cumulative = 0;
    for (int i = 0; i < Stats::kBuckets; i++) {
        cumulative += stats_.stepBuckets[i];
        char le[32];
        std::snprintf(le, sizeof(le), "le=\"%g\"", Stats::kBucketSeconds[i]);
        line("sfvr_keeper_step_seconds_bucket", le, static_cast<double>(cumulative));
    }
    line("sfvr_keeper_step_seconds_bucket", "le=\"+Inf\"", static_cast<double>(stats_.steps));
    line("sfvr_keeper_step_seconds_sum", "", stats_.stepSeconds);
    line("sfvr_keeper_step_seconds_count", "", static_cast<double>(stats_.steps));
    head("sfvr_keeper_step_max_seconds", "gauge", "The longest step since the last write of this file.");
    line("sfvr_keeper_step_max_seconds", "", stats_.stepMaxSeconds);
    stats_.stepMaxSeconds = 0;

    head("sfvr_keeper_requests_total", "counter", "Members' requests handled, by kind.");
    for (const auto& [kind, n] : stats_.requests) line("sfvr_keeper_requests_total", "kind=\"" + kind + "\"", static_cast<double>(n));
    head("sfvr_keeper_answers_total", "counter", "Answers to requests.");
    line("sfvr_keeper_answers_total", "", static_cast<double>(stats_.answers));
    head("sfvr_keeper_notices_total", "counter", "Words sent unasked (tickets, play-on answers).");
    line("sfvr_keeper_notices_total", "", static_cast<double>(stats_.notices));
    head("sfvr_keeper_resends_total", "counter", "Answers and notices sent again (not acknowledged within 250 ms).");
    line("sfvr_keeper_resends_total", "", static_cast<double>(stats_.resends));
    head("sfvr_keeper_tickets_total", "counter", "Match tickets sent to players.");
    line("sfvr_keeper_tickets_total", "", static_cast<double>(stats_.tickets));
    head("sfvr_keeper_calls_total", "counter", "Table calls (your-turn cards).");
    line("sfvr_keeper_calls_total", "", static_cast<double>(stats_.calls));
    head("sfvr_keeper_calls_lapsed_total", "counter", "Players who let a call lapse.");
    line("sfvr_keeper_calls_lapsed_total", "", static_cast<double>(stats_.callsLapsed));
    head("sfvr_keeper_sets_total", "counter", "Table sets ended, by how: result, forfeit, again (played on), unfinished.");
    for (const char* how : {"result", "forfeit", "again", "unfinished"}) {
        const auto it = stats_.sets.find(how);
        line("sfvr_keeper_sets_total", std::string("end=\"") + how + "\"", it != stats_.sets.end() ? static_cast<double>(it->second) : 0.0);
    }
    head("sfvr_keeper_rooms_asked_total", "counter", "Custom rooms asked for and being made.");
    line("sfvr_keeper_rooms_asked_total", "", static_cast<double>(stats_.roomsAsked));
    head("sfvr_keeper_rooms_closed_total", "counter", "Custom rooms closed.");
    line("sfvr_keeper_rooms_closed_total", "", static_cast<double>(stats_.roomsClosed));
    head("sfvr_keeper_kicks_total", "counter", "Members removed, by why.");
    line("sfvr_keeper_kicks_total", "why=\"flood\"", static_cast<double>(stats_.floodKicks));
    line("sfvr_keeper_kicks_total", "why=\"owner\"", static_cast<double>(stats_.ownerKicks));
    head("sfvr_keeper_server_lost_total", "counter", "Times the lobby's connection to the rooms server broke.");
    line("sfvr_keeper_server_lost_total", "", static_cast<double>(stats_.serverLost));
    head("sfvr_keeper_connects_total", "counter", "Connections the keeper opened to the rooms server (one per room and per set's relay room).");
    line("sfvr_keeper_connects_total", "", static_cast<double>(stats_.connects));
    head("sfvr_keeper_connect_failures_total", "counter", "Of those, connections that failed.");
    line("sfvr_keeper_connect_failures_total", "", static_cast<double>(stats_.connectFailures));
    head("sfvr_keeper_connect_seconds_total", "counter", "Time the loop spent opening them (it waits for each).");
    line("sfvr_keeper_connect_seconds_total", "", stats_.connectSeconds);
    head("sfvr_keeper_connects_pending", "gauge", "Connects asked of the connect workers and not over yet.");
    line("sfvr_keeper_connects_pending", "", static_cast<double>(connector_->Pending()));
    head("sfvr_keeper_connect_max_seconds", "gauge", "The longest connect since the last write of this file.");
    line("sfvr_keeper_connect_max_seconds", "", stats_.connectMaxSeconds);
    stats_.connectMaxSeconds = 0;
    head("sfvr_keeper_relays_total", "counter", "Spectator relays started (one per table set).");
    line("sfvr_keeper_relays_total", "", static_cast<double>(stats_.relays));

    // Gauges, as of now.
    int rooms = 0, members = 0, queued = 0, relays = 0, viewers = 0;
    size_t outbox = lobby_ ? lobby_->outbox.size() : 0;
    std::map<char, int> phases;
    for (const char p : {'o', 'c', 'n', 'p', 'a'}) phases[p] = 0;
    for (const auto& r : rooms_) {
        outbox += r->outbox.size();
        if (r->closing || !r->inRoom) continue;
        rooms++;
        members += Count(*r);
        for (const Table& t : r->tables) {
            phases[static_cast<char>(t.s.phase)]++;
            queued += static_cast<int>(t.queue.size());
        }
        for (const auto& rl : r->relays) {
            if (rl->ended) continue;
            relays++;
            if (rl->core) viewers += static_cast<int>(rl->core->Stats().viewers);
        }
    }
    head("sfvr_keeper_lobby_up", "gauge", "1 when the keeper hosts the desk (the lobby channel: room requests, the list's summary).");
    line("sfvr_keeper_lobby_up", "", lobby_ && lobby_->inRoom && lobby_->host ? 1.0 : 0.0);
    head("sfvr_keeper_lobby_members", "gauge", "Members of the desk: players with the custom rooms screen open (the hub alone is in no room).");
    line("sfvr_keeper_lobby_members", "", lobby_ && lobby_->inRoom ? Count(*lobby_) : 0.0);
    // Players online (the rooms API's `online`): from the server's connection
    // count while its stats file is fresh, else only the members of rooms.
    bool fromServer = false;
    const int online = PlayersOnline(&fromServer);
    head("sfvr_keeper_players_online", "gauge", "Players connected to the rooms server (on the hub, at the desk or in a room), as the rooms API gives them.");
    line("sfvr_keeper_players_online", "", online);
    head("sfvr_keeper_online_from_server", "gauge", "1 while players online come from the server's connection count; 0: its stats file is missing or stale (only room members are counted).");
    line("sfvr_keeper_online_from_server", "", fromServer ? 1.0 : 0.0);
    const ServerStats server = ReadServerStats();
    if (server.connections >= 0) {
        head("sfvr_keeper_server_connections", "gauge", "Connections to the rooms server (its --stats file): players, the keeper's own, match and relay connections.");
        line("sfvr_keeper_server_connections", "", static_cast<double>(server.connections));
    }
    if (server.ageSeconds >= 0) {
        head("sfvr_keeper_server_stats_age_seconds", "gauge", "How old the rooms server's stats file is (it writes it every 5 s).");
        line("sfvr_keeper_server_stats_age_seconds", "", server.ageSeconds);
    }
    head("sfvr_keeper_rooms", "gauge", "Custom rooms the keeper is in.");
    line("sfvr_keeper_rooms", "", rooms);
    head("sfvr_keeper_public_rooms", "gauge", "Custom rooms on the public list.");
    line("sfvr_keeper_public_rooms", "", static_cast<double>(PublicList().size()));
    head("sfvr_keeper_room_members", "gauge", "Members of custom rooms.");
    line("sfvr_keeper_room_members", "", members);
    head("sfvr_keeper_tables", "gauge", "Tables by phase: open, call, connecting, playing, after.");
    const std::pair<char, const char*> names[] = {{'o', "open"}, {'c', "call"}, {'n', "connecting"}, {'p', "playing"}, {'a', "after"}};
    for (const auto& [p, name] : names) line("sfvr_keeper_tables", std::string("phase=\"") + name + "\"", phases[p]);
    head("sfvr_keeper_queued", "gauge", "Members in table queues.");
    line("sfvr_keeper_queued", "", queued);
    head("sfvr_keeper_relays", "gauge", "Spectator relays running.");
    line("sfvr_keeper_relays", "", relays);
    head("sfvr_keeper_relay_viewers", "gauge", "Viewers watching through the relays.");
    line("sfvr_keeper_relay_viewers", "", viewers);
    head("sfvr_keeper_outbox", "gauge", "Answers and notices waiting for an acknowledgement.");
    line("sfvr_keeper_outbox", "", static_cast<double>(outbox));
    return out;
}
void Keeper::CheckName(Room& room, uint32_t id)
{
    if (room.kind != Kind::Custom || !room.settings.isPublic || o_.words.empty()) return;
    const auto m = room.members.find(id);
    if (m == room.members.end() || !NameRejected(m->second.name, o_.words)) return;
    if (std::find(room.masked.begin(), room.masked.end(), id) != room.masked.end()) return;
    room.masked.push_back(id);
    // "Player 1234": four random digits, none another masked member here has.
    int n = 0;
    do {
        n = 1000 + static_cast<int>(rng_() % 9000);
    } while (std::any_of(room.maskNumbers.begin(), room.maskNumbers.end(), [&](const auto& kv) { return kv.second == n; }));
    room.maskNumbers[id] = n;
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
        const auto n = room.maskNumbers.find(id);
        if (n != room.maskNumbers.end()) numbers[id] = n->second;
    }
    WriteData(room, kMask, FormatMask(numbers));
}

void Keeper::WriteWordList(Room& room)
{
    if (room.kind != Kind::Custom || !room.host) return;
    const std::vector<std::string> values = room.settings.isPublic ? PackWordList(o_.words) : std::vector<std::string>{};
    for (int i = 0; i < kWordListKeys; i++) {
        if (i < static_cast<int>(values.size())) WriteData(room, WordListKey(i), values[i]);
        else if (room.written.count(WordListKey(i))) DeleteData(room, WordListKey(i));
    }
    if (values.empty()) {
        if (room.written.count(kWordListHash)) DeleteData(room, kWordListHash);
    }
    else WriteData(room, kWordListHash, WordListHash(o_.words));
}

void Keeper::SetWords(std::vector<std::string> words)
{
    o_.words = std::move(words);
    Log("keeper: word list: %zu entries", o_.words.size());
    for (auto& r : rooms_) {
        if (r->closing || !r->inRoom) continue;
        WriteWordList(*r);
        for (uint32_t id : r->order) CheckName(*r, id);
    }
}

std::string Keeper::ShownName(const Room& room, uint32_t id) const
{
    const auto m = room.members.find(id);
    if (m == room.members.end()) return {};
    if (std::find(room.masked.begin(), room.masked.end(), id) == room.masked.end()) return m->second.name;
    // "Player N": N its place in the room (join order), as every client draws it.
    const auto n = room.maskNumbers.find(id);
    return "Player " + std::to_string(n != room.maskNumbers.end() ? n->second : 0);
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
        if (p != room->members.end()) d.people.push_back({ShownName(*room, id), p->second.chara, p->second.country});
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
        stats_.calls++;
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
        t.s.again = t.s.phase == TablePhase::Playing && t.queue.empty() && room.members.count(t.s.seat[0]) &&
                    room.members.count(t.s.seat[1]);
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
    stats_.tickets++;
    Log("keeper: room %s: ticket to %u: %s", room.code.c_str(), to, FormatTicket(ticket).c_str());
    Notice(room, to, "ticket", true, FormatTicket(ticket), nowMs);
}

void Keeper::Notice(Room& room, uint32_t to, const std::string& kind, bool ok, const std::string& text, int64_t nowMs)
{
    // Sent again until acknowledged, as an answer is; numbered after the
    // player's last request, so their acknowledgement covers it.
    const auto m = room.members.find(to);
    const uint32_t seq = m != room.members.end() ? m->second.lastSeq + 1000000 : 1;
    stats_.notices++;
    room.outbox.push_back({to, seq, FormatAnswer({seq, kind, ok, text}), nowMs, nowMs + kConnectMs});
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
    if (!room.host) { // paused: nothing moves
        Log("keeper: room %s: %s (%u) asks %s while the room is paused; ignored", room.code.c_str(), m.name.c_str(), m.id, FormatRequest(r).c_str());
        return;
    }
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
        if (seat < 0) Log("keeper: room %s: %s (%u) readies at table %d but is not seated there; ignored", room.code.c_str(), m.name.c_str(), m.id, index + 1);
        if (seat >= 0) {
            t->s.ready[seat] = true;
            m.callStrikes = 0;
            Log("keeper: room %s: table %d: %s (%u) is ready as P%d%s", room.code.c_str(), index + 1, m.name.c_str(), m.id, seat + 1,
                t->s.ready[0] && t->s.ready[1] ? ": both ready, P1 opens the match room" : "");
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
    else if (r.kind == "code" && t && (r.args.size() == 2 || r.args.size() == 3) && t->s.phase == TablePhase::Connecting &&
             seatOf(*t) == 0) {
        t->s.code = r.args[1];
        t->build = r.args.size() == 3 ? r.args[2] : std::string();
        Log("keeper: room %s: table %d: P1 opened match room %s (build %s); P2's ticket goes", room.code.c_str(), index + 1, t->s.code.c_str(),
            t->build.empty() ? "-" : t->build.c_str());
        t->s.watch.clear();
        SetPhase(*t, TablePhase::Playing, nowMs);
        Ticket(room, t->s.seat[1], {false, index, t->s.code, t->token, FormatRules(t->setRules)}, nowMs);
        StartRelay(room, index, nowMs);
    }
    else if ((r.kind == "score" || r.kind == "result") && t && r.args.size() == 3 &&
             (t->s.phase == TablePhase::Playing || t->s.phase == TablePhase::Connecting)) {
        // P1 reports; P2 only once P1 is gone.
        const int seat = seatOf(*t);
        if (seat == 0 || (seat == 1 && !room.members.count(t->s.seat[0]))) {
            const uint8_t games[2] = {static_cast<uint8_t>(std::clamp(std::atoi(r.args[1].c_str()), 0, 99)),
                                      static_cast<uint8_t>(std::clamp(std::atoi(r.args[2].c_str()), 0, 99))};
            if (r.kind == "score") {
                if (t->s.wins[0] != games[0] || t->s.wins[1] != games[1])
                    Log("keeper: room %s: table %d: games %d-%d", room.code.c_str(), index + 1, games[0], games[1]);
                t->s.wins[0] = games[0];
                t->s.wins[1] = games[1];
            }
            else if (games[0] == games[1]) {
                stats_.sets["unfinished"]++;
                Log("keeper: room %s: table %d's set did not finish; calling it again", room.code.c_str(), index + 1);
                Fill(room, *t, nowMs, true);
            }
            else {
                ApplySetResult(t->setRules, games[0] > games[1] ? 0 : 1, games, &t->s, &t->queue);
                t->s.watch.clear(); // the set's relay serves its viewers on; nobody new
                t->phaseAtMs = nowMs;
                stats_.sets["result"]++;
                Log("keeper: room %s: table %d: %u beat %u %d-%d", room.code.c_str(), index + 1, t->s.lastWinner, t->s.lastLoser,
                    t->s.lastScore[0], t->s.lastScore[1]);
            }
        }
    }
    else if (r.kind == "forfeit" && t && r.args.size() == 2 &&
             (t->s.phase == TablePhase::Playing || t->s.phase == TablePhase::Connecting)) {
        // A player left the set (both games say so; the first counts): the
        // other wins it, the rotation runs, and the one who left is off the
        // table and its queue (user, 2026-11-04: a forfeit, not queued again).
        const int seat = std::atoi(r.args[1].c_str());
        if (seatOf(*t) >= 0 && (seat == 0 || seat == 1) && t->s.seat[0] && t->s.seat[1]) {
            const uint32_t leaver = t->s.seat[seat];
            const uint8_t games[2] = {t->s.wins[0], t->s.wins[1]};
            ApplySetResult(t->setRules, 1 - seat, games, &t->s, &t->queue);
            t->s.watch.clear();
            LeaveTable(leaver, &t->s, &t->queue);
            t->phaseAtMs = nowMs;
            stats_.sets["forfeit"]++;
            Log("keeper: room %s: table %d: %u beat %u by forfeit (%d-%d)", room.code.c_str(), index + 1, t->s.lastWinner,
                t->s.lastLoser, t->s.lastScore[0], t->s.lastScore[1]);
        }
    }
    else if (r.kind == "again" && t && r.args.size() == 3 && t->s.phase == TablePhase::Playing && seatOf(*t) == 0) {
        // The set is won and both want more. Nobody else waits for the
        // table: the result goes on record and the same two play the next
        // set in the same match room, in the same seats (the rotation only
        // matters with someone waiting; the stays limit does not count it).
        const uint8_t games[2] = {static_cast<uint8_t>(std::clamp(std::atoi(r.args[1].c_str()), 0, 99)),
                                  static_cast<uint8_t>(std::clamp(std::atoi(r.args[2].c_str()), 0, 99))};
        const bool alone = t->queue.empty() && room.members.count(t->s.seat[0]) && room.members.count(t->s.seat[1]);
        if (alone && games[0] != games[1]) {
            const int w = games[0] > games[1] ? 0 : 1;
            t->s.lastWinner = t->s.seat[w];
            t->s.lastLoser = t->s.seat[1 - w];
            t->s.lastScore[0] = games[w];
            t->s.lastScore[1] = games[1 - w];
            t->s.wins[0] = t->s.wins[1] = 0;
            t->setRules = t->rules;
            t->phaseAtMs = nowMs;
            stats_.sets["again"]++;
            Log("keeper: room %s: table %d: %u beat %u %d-%d; nobody waiting, the same two play on (%s)", room.code.c_str(),
                index + 1, t->s.lastWinner, t->s.lastLoser, t->s.lastScore[0], t->s.lastScore[1], FormatRules(t->setRules).c_str());
            for (int seat = 0; seat < 2; seat++) Notice(room, t->s.seat[seat], "again", true, FormatRules(t->setRules), nowMs);
            // Its relay carries on (the same match room) to the next set's end.
            for (auto& rl : room.relays) {
                if (!rl->ended && rl->matchCode == t->s.code && rl->core) rl->core->SetFirstTo(t->setRules.firstTo);
            }
        }
        else {
            // Someone waits (or it did not finish): the set ends as a result does.
            if (games[0] == games[1]) {
                stats_.sets["unfinished"]++;
                Log("keeper: room %s: table %d's set did not finish; calling it again", room.code.c_str(), index + 1);
                Fill(room, *t, nowMs, true);
            }
            else {
                stats_.sets["result"]++;
                ApplySetResult(t->setRules, games[0] > games[1] ? 0 : 1, games, &t->s, &t->queue);
                t->s.watch.clear();
                t->phaseAtMs = nowMs;
                Log("keeper: room %s: table %d: %u beat %u %d-%d (someone is waiting: no next set for the same two)", room.code.c_str(),
                    index + 1, t->s.lastWinner, t->s.lastLoser, t->s.lastScore[0], t->s.lastScore[1]);
            }
            Notice(room, m.id, "again", false, "someone is waiting", nowMs);
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
            stats_.ownerKicks++;
            Log("keeper: room %s: the owner removes %s (%u)", room.code.c_str(), room.members[id].name.c_str(), id);
            // Barred until they reconnect (Weyvelength's ban holds the connection).
            weyve_ban_member(room.client, id);
        }
    }
    else {
        // Nothing matched: a request for a phase the table has left, a seat
        // it does not hold, or a table that is not there (races show here).
        Log("keeper: room %s: %s (%u) asks %s; ignored (table %d is %s)", room.code.c_str(), m.name.c_str(), m.id, FormatRequest(r).c_str(),
            index + 1, t ? FormatTable(t->s).c_str() : "not there");
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
                stats_.callsLapsed++;
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
            const std::string w = WordEntry(arg);
            if (!w.empty() && std::find(o_.words.begin(), o_.words.end(), w) == o_.words.end()) {
                std::vector<std::string> words = o_.words;
                words.push_back(w);
                Log("keeper: operator: '%s' added to the word filter", w.c_str());
                SetWords(std::move(words));
            }
        }
        else if (cmd == "rooms") {
            Log("keeper: operator: lobby %s, %zu custom rooms", lobby_ ? lobby_->code.c_str() : "-", rooms_.size());
            for (const auto& r : rooms_) {
                Log("keeper:   %s %s %s, %d members, %d tables%s%s", r->code.c_str(), FormatTitle(r->settings.title).c_str(),
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
    stats_.roomsClosed++;
    Log("keeper: closing room %s: %s", room.code.empty() ? "(new)" : room.code.c_str(), why);
    room.closing = true;
    for (auto& r : room.relays) EndRelay(room, *r, "the room closed");
    room.relays.clear();
    if (room.requester && lobby_) {
        // Asked for and never made.
        Answer(*lobby_, room.requester, {room.requestSeq, "room", false, "the room could not be made"}, now_);
        room.requester = 0;
    }
    connector_->Cancel(room.connecting);
    room.connecting = 0;
    if (room.listedOn) listRev_++; // its listing goes with the room
    room.listedOn = false;
    if (room.client) Retire(room.client);
}

// ----- spectator relays (core/spectate_relay.h) ----------------------------------

void Keeper::StartRelay(Room& room, int table, int64_t nowMs)
{
    if (table < 0 || table >= static_cast<int>(room.tables.size())) return;
    Table& t = room.tables[table];
    auto r = std::make_unique<RelayLink>();
    r->table = table;
    r->matchCode = t.s.code;
    r->startedMs = nowMs;
    r->statsMs = nowMs;
    // Its room is made once the connect comes through (PumpRelays).
    Ask(r->downConnecting, r->downAskedMs, nowMs);
    // What viewers check before they say hello (the match room's own data
    // once the relay is in it). Without P1's build (an older report), the
    // relay joins the match room at once to learn it.
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
    else r->upStartedMs = -2; // joins at once (PumpRelays)
    stats_.relays++;
    Log("keeper: room %s: table %d: relay for match room %s", room.code.c_str(), table + 1, r->matchCode.c_str());
    room.relays.push_back(std::move(r));
}

void Keeper::PumpRelays(Room& room, int64_t nowMs)
{
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
        // The first viewer: into the match room (the host keeps the set's
        // games, so a viewer who comes late still starts at the first).
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
                }
                else {
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
            Log("keeper: room %s: table %d: relay %s: %zu viewers, %zu games, %llu KB from the host (%llu KB of states), %llu KB to viewers",
                room.code.c_str(), r->table + 1, r->relayCode.c_str(), s.viewers, s.games,
                static_cast<unsigned long long>(s.bytesFromHost / 1024), static_cast<unsigned long long>(s.stateBytesFromHost / 1024),
                static_cast<unsigned long long>(s.bytesToViewers / 1024));
            if (s.viewers) Log("keeper: room %s: table %d: relay %s viewers: %s", room.code.c_str(), r->table + 1, r->relayCode.c_str(),
                               r->core->ViewersLine().c_str());
        }
        // Done; the host never let it in; or the set is over and nobody watched.
        const bool setOn = r->table >= 0 && r->table < static_cast<int>(room.tables.size()) &&
                           room.tables[r->table].s.code == r->matchCode &&
                           (room.tables[r->table].s.phase == TablePhase::Playing || room.tables[r->table].s.phase == TablePhase::Connecting);
        if (r->core->Done(nowMs)) EndRelay(room, *r, "done");
        else if (r->upStartedMs >= 0 && !r->core->Stats().hostAccepted && nowMs - r->upStartedMs > 60000) EndRelay(room, *r, "the host never answered");
        else if (r->upStartedMs < 0 && !setOn) EndRelay(room, *r, "the set is over; nobody watched");
    }
    std::erase_if(room.relays, [](const std::unique_ptr<RelayLink>& r) { return r->ended; });
}

void Keeper::PumpRelayUp(Room& room, RelayLink& r, int64_t nowMs)
{
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
            // The proof the host checks before it lets this member watch.
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
            // The match room's own words for it: the relay room says the same
            // (a viewer's message names the side to update when builds differ).
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

void Keeper::PumpRelayDown(Room& room, RelayLink& r, int64_t nowMs)
{
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
            // No Polarity build chats here (the match room's rule, M6).
            if (e.data.chat.from != self) weyve_ban_member(c, e.data.chat.from);
            break;
        default:
            break;
        }
    }
    // The relay room says what the match room says of the build, so viewers
    // check it as they would the host's; then it is the table's to watch.
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

void Keeper::EndRelay(Room& room, RelayLink& r, const char* why)
{
    if (r.ended) return;
    r.ended = true;
    const RelayStats s = r.core ? r.core->Stats() : RelayStats{};
    Log("keeper: room %s: table %d: relay %s ends (%s): %zu games, %llu KB from the host (%llu KB of states), %llu KB to viewers",
        room.code.c_str(), r.table + 1, r.relayCode.c_str(), why, s.games, static_cast<unsigned long long>(s.bytesFromHost / 1024),
        static_cast<unsigned long long>(s.stateBytesFromHost / 1024), static_cast<unsigned long long>(s.bytesToViewers / 1024));
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
