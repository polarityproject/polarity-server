#include "keeper/keeper_internal.h"

namespace sfv::keeper {

using namespace internal;

namespace internal {

std::string Str(const char* p, uint32_t len) { return p ? std::string(p, len) : std::string(); }

void SendLobby(WeyveClient* c, uint64_t to, const lobby::Message& m) {
    if (!c) return;
    const std::vector<uint8_t> bytes = lobby::Encode(m);
    std::vector<uint8_t> tagged(bytes.size() + 1);
    tagged[0] = kControlChannel;
    std::copy(bytes.begin(), bytes.end(), tagged.begin() + 1);
    weyve_send_p2p(c, static_cast<uint32_t>(to), tagged.data(), static_cast<uint32_t>(tagged.size()));
}

const char* RoomErrorText(WeyveRoomError e) {
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

} // namespace internal

namespace {

constexpr int64_t kReconnectMs = 5000;

constexpr int64_t kConnectWaitMs = 30000;

constexpr int kConnectThreads = 4;

} // namespace

Keeper::Keeper(KeeperOptions options) : o_(std::move(options)) {
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

Keeper::~Keeper() {
    for (auto& r : rooms_) {
        for (auto& relay : r->relays) {
            if (relay->up) weyve_client_destroy(relay->up);
            if (relay->down) weyve_client_destroy(relay->down);
        }
        if (r->client) weyve_client_destroy(r->client);
    }
    if (lobby_ && lobby_->client) weyve_client_destroy(lobby_->client);
    connector_.reset();
}

void Keeper::Log(const char* format, ...) const {
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

WeyveClient* Keeper::NewClient() const {
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

void Keeper::Ask(uint64_t& ticket, int64_t& askedMs, int64_t nowMs) {
    if (ticket) return;
    ticket = connector_->Request();
    askedMs = nowMs;
}

bool Keeper::Arrived(uint64_t& ticket, int64_t askedMs, int64_t nowMs, WeyveClient** client) {
    if (!ticket) return false;
    double took = 0;
    if (!connector_->Take(ticket, client, &took)) {
        if (nowMs - askedMs < kConnectWaitMs) return false;
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

void Keeper::Retire(WeyveClient*& client) {
    connector_->Retire(client);
    client = nullptr;
}

bool Keeper::Start(int64_t nowMs) {
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
    Log("keeper: connecting to %s:%u", o_.host.c_str(), o_.port);
    Ask(lobby_->connecting, lobby_->connectAskedMs, nowMs);
    for (auto& r : rooms_) Ask(r->connecting, r->connectAskedMs, nowMs);
    return true;
}

void Keeper::ConnectedLobby(int64_t nowMs) {
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
    } else {
        FindLobby(*lobby_);
    }
    lobby_->joinAsked = true;
}

void Keeper::ConnectedRoom(Room& room, int64_t nowMs) {
    (void)nowMs;
    if (room.reclaimed) {
        Log("keeper: rejoining custom room %s", room.code.c_str());
        weyve_join_room(room.client, room.code.c_str(), nullptr);
    } else {
        weyve_create_room(room.client);
    }
    room.joinAsked = true;
}

void Keeper::Step(int64_t nowMs) {
    now_ = nowMs;
    if (!lobby_) return;
    const auto stepStart = std::chrono::steady_clock::now();
    if (lobby_->connecting) {
        WeyveClient* c = nullptr;
        if (Arrived(lobby_->connecting, lobby_->connectAskedMs, nowMs, &c)) {
            if (c) {
                lobby_->client = c;
                ConnectedLobby(nowMs);
            } else {
                Log("keeper: cannot connect to %s:%u; trying again in %lld s", o_.host.c_str(), o_.port,
                    static_cast<long long>(kReconnectMs / 1000));
                lobby_->reconnectMs = nowMs + kReconnectMs;
            }
        }
    } else if (!lobby_->client && nowMs >= lobby_->reconnectMs) {
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

void Keeper::Pump(Room& room, int64_t nowMs) {
    WeyveClient* c = room.client;
    if (!weyve_poll(c)) {
        if (room.kind == Kind::Lobby) {
            stats_.serverLost++;
            Log("keeper: lost the server (lobby channel %s); reconnecting in %lld s", room.code.c_str(),
                static_cast<long long>(kReconnectMs / 1000));
            Retire(room.client);
            room.inRoom = room.host = room.setUp = false;
            room.reconnectMs = nowMs + kReconnectMs;
        } else {
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
                    } else {
                        Close(room, "gone");
                    }
                } else if (!room.inRoom && room.kind == Kind::Lobby && code == WEYVE_ROOM_ERROR_RATE_LIMITED) {
                    // The server's wait after a failed join (or a list asked too soon). A
                    // new connection starts without it and rejoins room.code, or looks again.
                    Log("keeper: lobby channel asked too soon; reconnecting in %lld s", static_cast<long long>(kReconnectMs / 1000));
                    Retire(room.client);
                    room.host = room.setUp = false;
                    room.reconnectMs = nowMs + kReconnectMs;
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
                } else {
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
    uint32_t from = 0, len = 0;
    while (weyve_next_p2p(c, &from, &len)) {
    }
}

void Keeper::Commands(int64_t nowMs) {
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
            listWrittenMs_ = -1;
            Log("keeper: operator: %s %s%s", cmd.c_str(), arg.c_str(), found ? "" : " (no such room)");
        } else if (cmd == "word" && !arg.empty()) {
            const std::string w = WordEntry(arg);
            if (!w.empty() && std::find(o_.words.begin(), o_.words.end(), w) == o_.words.end()) {
                std::vector<std::string> words = o_.words;
                words.push_back(w);
                Log("keeper: operator: '%s' added to the word filter", w.c_str());
                SetWords(std::move(words));
            }
        } else if (cmd == "rooms") {
            Log("keeper: operator: lobby %s, %zu custom rooms", lobby_ ? lobby_->code.c_str() : "-", rooms_.size());
            for (const auto& r : rooms_) {
                Log("keeper:   %s %s %s, %d members, %d tables%s%s", r->code.c_str(), FormatTitle(r->settings.title).c_str(),
                    r->settings.isPublic ? "public" : "private", Count(*r), r->settings.tables, r->unlisted ? ", unlisted" : "",
                    r->host ? "" : ", paused");
            }
        } else {
            Log("keeper: operator: unknown command '%s'", line.c_str());
        }
    }
}

std::vector<std::string> Keeper::LoadState() {
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

void Keeper::SaveState() {
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
