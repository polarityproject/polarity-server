#include "keeper/keeper_internal.h"

namespace sfv::keeper {

using namespace internal;

std::string Keeper::SignText(const std::string& message) const {
    uint8_t signature[64];
    if (!Sign(o_.key, reinterpret_cast<const uint8_t*>(message.data()), message.size(), signature)) return {};
    return Base64Encode(signature, sizeof(signature));
}

void Keeper::WriteListings(int64_t nowMs) {
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

std::vector<ListedRoom> Keeper::PublicList() const {
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
        for (const Table& t : r->tables) {
            ListedRoom::TableEntry e;
            e.rules = FormatRules(t.rules);
            switch (t.s.phase) {
                case TablePhase::Open: e.state = "open"; break;
                case TablePhase::Call: e.state = "called"; break;
                case TablePhase::Connecting: e.state = "connecting"; break;
                case TablePhase::Playing: e.state = "playing"; break;
                case TablePhase::After: e.state = "after"; break;
            }
            for (int i = 0; i < 2; i++) {
                const uint32_t id = t.s.seat[i];
                const auto m = id ? r->members.find(id) : r->members.end();
                if (m == r->members.end()) continue;
                e.seat[i] = {ShownName(*r, id), m->second.country, m->second.chara};
                e.wins[i] = t.s.wins[i];
            }
            l.tableList.push_back(std::move(e));
        }
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

Keeper::ServerStats Keeper::ReadServerStats() const {
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

int Keeper::OnlineFromServer(const ServerStats& stats) const {
    if (!stats.Fresh()) return -1;
    const long connections = stats.connections;
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

int Keeper::PlayersOnline(bool* fromServer) const {
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

std::string Keeper::StatusJson(int64_t unixMs) const {
    RoomTotals totals;
    bool fromServer = false;
    totals.online = PlayersOnline(&fromServer);
    const std::vector<ListedRoom> list = PublicList();
    totals.publicRooms = static_cast<int>(list.size());
    for (const ListedRoom& l : list) totals.inPublicRooms += l.members;
    return FormatRoomListJson(list, totals, unixMs);
}

std::string Keeper::MetricsText() {
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

void Keeper::CheckName(Room& room, uint32_t id) {
    if (room.kind != Kind::Custom || !room.settings.isPublic || o_.words.empty()) return;
    const auto m = room.members.find(id);
    if (m == room.members.end() || !NameRejected(m->second.name, o_.words)) return;
    if (std::find(room.masked.begin(), room.masked.end(), id) != room.masked.end()) return;
    room.masked.push_back(id);
    int n = 0;
    do {
        n = 1000 + static_cast<int>(rng_() % 9000);
    } while (std::any_of(room.maskNumbers.begin(), room.maskNumbers.end(), [&](const auto& kv) { return kv.second == n; }));
    room.maskNumbers[id] = n;
    Log("keeper: room %s: %u's name shows as %s here", room.code.c_str(), id, ShownName(room, id).c_str());
    WriteMask(room);
}

void Keeper::WriteMask(Room& room) {
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

void Keeper::WriteWordList(Room& room) {
    if (room.kind != Kind::Custom || !room.host) return;
    const std::vector<std::string> values = room.settings.isPublic ? PackWordList(o_.words) : std::vector<std::string>{};
    for (int i = 0; i < kWordListKeys; i++) {
        if (i < static_cast<int>(values.size())) WriteData(room, WordListKey(i), values[i]);
        else if (room.written.count(WordListKey(i))) DeleteData(room, WordListKey(i));
    }
    if (values.empty()) {
        if (room.written.count(kWordListHash)) DeleteData(room, kWordListHash);
    } else WriteData(room, kWordListHash, WordListHash(o_.words));
}

void Keeper::SetWords(std::vector<std::string> words) {
    o_.words = std::move(words);
    Log("keeper: word list: %zu entries", o_.words.size());
    for (auto& r : rooms_) {
        if (r->closing || !r->inRoom) continue;
        WriteWordList(*r);
        for (uint32_t id : r->order) CheckName(*r, id);
    }
}

std::string Keeper::ShownName(const Room& room, uint32_t id) const {
    const auto m = room.members.find(id);
    if (m == room.members.end()) return {};
    if (std::find(room.masked.begin(), room.masked.end(), id) == room.masked.end()) return m->second.name;
    const auto n = room.maskNumbers.find(id);
    return "Player " + std::to_string(n != room.maskNumbers.end() ? n->second : 0);
}

void Keeper::Peek(Room& lobby, Member& m, const Request& request, int64_t nowMs) {
    if (m.lastPeekMs >= 0 && nowMs - m.lastPeekMs < kPeekEveryMs) return;
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

} // namespace sfv::keeper
