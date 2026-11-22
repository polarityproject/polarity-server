#include "keeper/keeper_internal.h"

namespace sfv::keeper {

using namespace internal;

void Keeper::SetUpTables(Room& room, int64_t nowMs) {
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
        std::erase_if(table.queue, [&](uint32_t id) { return !room.members.count(id); });
        for (uint32_t& id : table.s.seat) {
            if (id && !room.members.count(id)) id = 0;
        }
        table.setRules = table.rules;
        table.phaseAtMs = nowMs;
        table.written.clear();
        if (table.s.phase == TablePhase::Call || table.s.phase == TablePhase::Connecting || table.s.phase == TablePhase::After) {
            Fill(room, table, nowMs, table.s.phase == TablePhase::Connecting);
        }
    }
    WriteTables(room);
}

void Keeper::SetPhase(Table& t, TablePhase phase, int64_t nowMs) {
    t.s.phase = phase;
    t.phaseAtMs = nowMs;
}

void Keeper::Fill(Room& room, Table& t, int64_t nowMs, bool brokeOff) {
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

void Keeper::WriteTables(Room& room) {
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

void Keeper::Ticket(Room& room, uint32_t to, const TableTicket& ticket, int64_t nowMs) {
    stats_.tickets++;
    Log("keeper: room %s: ticket to %u: %s", room.code.c_str(), to, FormatTicket(ticket).c_str());
    Notice(room, to, "ticket", true, FormatTicket(ticket), nowMs);
}

void Keeper::Notice(Room& room, uint32_t to, const std::string& kind, bool ok, const std::string& text, int64_t nowMs) {
    const auto m = room.members.find(to);
    const uint32_t seq = m != room.members.end() ? m->second.lastSeq + 1000000 : 1;
    stats_.notices++;
    room.outbox.push_back({to, seq, FormatAnswer({seq, kind, ok, text}), nowMs, nowMs + kConnectMs});
    SendOutbox(room, nowMs);
}

void Keeper::OffTables(Room& room, uint32_t id, int64_t nowMs) {
    for (Table& t : room.tables) {
        const bool seated = t.s.seat[0] == id || t.s.seat[1] == id;
        const bool inSet = t.s.phase == TablePhase::Connecting || t.s.phase == TablePhase::Playing;
        if (!LeaveTable(id, &t.s, &t.queue)) continue;
        if (seated && t.s.phase != TablePhase::Open) {
            Fill(room, t, nowMs, inSet);
        } else if (t.s.phase == TablePhase::Open) {
            Fill(room, t, nowMs);
        }
    }
    WriteTables(room);
}

void Keeper::TableRequest(Room& room, Member& m, const Request& r, int64_t nowMs) {
    if (!room.host) {
        Log("keeper: room %s: %s (%u) asks %s while the room is paused; ignored", room.code.c_str(), m.name.c_str(), m.id, FormatRequest(r).c_str());
        return;
    }
    const int count = static_cast<int>(room.tables.size());
    int index = -1;
    if (!r.args.empty()) index = std::atoi(r.args[0].c_str());
    Table* t = index >= 0 && index < count ? &room.tables[index] : nullptr;
    auto seatOf = [&](const Table& table) { return table.s.seat[0] == m.id ? 0 : table.s.seat[1] == m.id ? 1
                                                                                                         : -1; };
    if (r.kind == "seat" && t && r.args.size() == 2) {
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
    } else if (r.kind == "stand") {
        for (Table& table : room.tables) {
            if (seatOf(table) < 0) continue;
            if (table.s.phase == TablePhase::Open || table.s.phase == TablePhase::Call) {
                LeaveTable(m.id, &table.s, &table.queue);
                Fill(room, table, nowMs);
            } else if (table.s.phase == TablePhase::Connecting || table.s.phase == TablePhase::Playing) {
                LeaveTable(m.id, &table.s, &table.queue);
                Log("keeper: room %s: %s leaves table %d before its match", room.code.c_str(), m.name.c_str(),
                    static_cast<int>(&table - room.tables.data()) + 1);
                Fill(room, table, nowMs, true);
            }
        }
    } else if (r.kind == "queue" && t) {
        for (Table& other : room.tables) {
            if (&other != t && LeaveTable(m.id, &other.s, &other.queue) && other.s.phase != TablePhase::Open) Fill(room, other, nowMs);
        }
        if (seatOf(*t) < 0 && std::find(t->queue.begin(), t->queue.end(), m.id) == t->queue.end()) {
            t->queue.push_back(m.id);
            m.callStrikes = 0;
            Log("keeper: room %s: %s queues for table %d (%zu in queue)", room.code.c_str(), m.name.c_str(), index + 1, t->queue.size());
            if (t->s.phase == TablePhase::Open) Fill(room, *t, nowMs);
        }
    } else if (r.kind == "unqueue") {
        for (Table& table : room.tables) std::erase(table.queue, m.id);
    } else if (r.kind == "ready" && t && t->s.phase == TablePhase::Call) {
        const int seat = seatOf(*t);
        if (seat < 0) Log("keeper: room %s: %s (%u) readies at table %d but is not seated there; ignored", room.code.c_str(), m.name.c_str(), m.id, index + 1);
        if (seat >= 0) {
            t->s.ready[seat] = true;
            m.callStrikes = 0;
            Log("keeper: room %s: table %d: %s (%u) is ready as P%d%s", room.code.c_str(), index + 1, m.name.c_str(), m.id, seat + 1,
                t->s.ready[0] && t->s.ready[1] ? ": both ready, P1 opens the match room" : "");
            if (t->s.ready[0] && t->s.ready[1]) {
                t->token = rng_() | 1;
                SetPhase(*t, TablePhase::Connecting, nowMs);
                Ticket(room, t->s.seat[0], {true, index, "", t->token, FormatRules(t->setRules)}, nowMs);
            }
        }
    } else if (r.kind == "decline" && t && seatOf(*t) >= 0 &&
               (t->s.phase == TablePhase::Call || t->s.phase == TablePhase::Open)) {
        LeaveTable(m.id, &t->s, &t->queue);
        Log("keeper: room %s: %s leaves table %d's queue", room.code.c_str(), m.name.c_str(), index + 1);
        Fill(room, *t, nowMs);
    } else if (r.kind == "code" && t && (r.args.size() == 2 || r.args.size() == 3) && t->s.phase == TablePhase::Connecting &&
               seatOf(*t) == 0) {
        t->s.code = r.args[1];
        t->build = r.args.size() == 3 ? r.args[2] : std::string();
        Log("keeper: room %s: table %d: P1 opened match room %s (build %s); P2's ticket goes", room.code.c_str(), index + 1, t->s.code.c_str(),
            t->build.empty() ? "-" : t->build.c_str());
        t->s.watch.clear();
        SetPhase(*t, TablePhase::Playing, nowMs);
        Ticket(room, t->s.seat[1], {false, index, t->s.code, t->token, FormatRules(t->setRules)}, nowMs);
        StartRelay(room, index, nowMs);
    } else if ((r.kind == "score" || r.kind == "result") && t && r.args.size() == 3 &&
               (t->s.phase == TablePhase::Playing || t->s.phase == TablePhase::Connecting)) {
        const int seat = seatOf(*t);
        if (seat == 0 || (seat == 1 && !room.members.count(t->s.seat[0]))) {
            const uint8_t games[2] = {static_cast<uint8_t>(std::clamp(std::atoi(r.args[1].c_str()), 0, 99)),
                                      static_cast<uint8_t>(std::clamp(std::atoi(r.args[2].c_str()), 0, 99))};
            if (r.kind == "score") {
                if (t->s.wins[0] != games[0] || t->s.wins[1] != games[1])
                    Log("keeper: room %s: table %d: games %d-%d", room.code.c_str(), index + 1, games[0], games[1]);
                t->s.wins[0] = games[0];
                t->s.wins[1] = games[1];
            } else if (games[0] == games[1]) {
                stats_.sets["unfinished"]++;
                Log("keeper: room %s: table %d's set did not finish; calling it again", room.code.c_str(), index + 1);
                Fill(room, *t, nowMs, true);
            } else {
                ApplySetResult(t->setRules, games[0] > games[1] ? 0 : 1, games, &t->s, &t->queue);
                t->s.watch.clear();
                t->phaseAtMs = nowMs;
                stats_.sets["result"]++;
                Log("keeper: room %s: table %d: %u beat %u %d-%d", room.code.c_str(), index + 1, t->s.lastWinner, t->s.lastLoser,
                    t->s.lastScore[0], t->s.lastScore[1]);
            }
        }
    } else if (r.kind == "forfeit" && t && r.args.size() == 2 &&
               (t->s.phase == TablePhase::Playing || t->s.phase == TablePhase::Connecting)) {
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
    } else if (r.kind == "again" && t && r.args.size() == 3 && t->s.phase == TablePhase::Playing && seatOf(*t) == 0) {
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
            for (auto& rl : room.relays) {
                if (!rl->ended && rl->matchCode == t->s.code && rl->core) rl->core->SetFirstTo(t->setRules.firstTo);
            }
        } else {
            if (games[0] == games[1]) {
                stats_.sets["unfinished"]++;
                Log("keeper: room %s: table %d's set did not finish; calling it again", room.code.c_str(), index + 1);
                Fill(room, *t, nowMs, true);
            } else {
                stats_.sets["result"]++;
                ApplySetResult(t->setRules, games[0] > games[1] ? 0 : 1, games, &t->s, &t->queue);
                t->s.watch.clear();
                t->phaseAtMs = nowMs;
                Log("keeper: room %s: table %d: %u beat %u %d-%d (someone is waiting: no next set for the same two)", room.code.c_str(),
                    index + 1, t->s.lastWinner, t->s.lastLoser, t->s.lastScore[0], t->s.lastScore[1]);
            }
            Notice(room, m.id, "again", false, "someone is waiting", nowMs);
        }
    } else if (r.kind == "rules" && t && r.args.size() == 2 && m.id == room.owner) {
        Rules rules;
        if (ParseRules(r.args[1], &rules)) {
            t->rules = rules;
            WriteData(room, TableKey(index), FormatRules(rules));
            Log("keeper: room %s: table %d's rules are %s from its next set", room.code.c_str(), index + 1, r.args[1].c_str());
        } else {
            Log("keeper: room %s: table %d: rules '%s' do not parse; kept", room.code.c_str(), index + 1, r.args[1].c_str());
        }
    } else if (r.kind == "kick" && !r.args.empty() && m.id == room.owner) {
        const uint32_t id = static_cast<uint32_t>(std::strtoul(r.args[0].c_str(), nullptr, 10));
        if (id && id != m.id && room.members.count(id)) {
            stats_.ownerKicks++;
            Log("keeper: room %s: the owner removes %s (%u)", room.code.c_str(), room.members[id].name.c_str(), id);
            weyve_ban_member(room.client, id);
        }
    } else {
        Log("keeper: room %s: %s (%u) asks %s; ignored (table %d is %s)", room.code.c_str(), m.name.c_str(), m.id, FormatRequest(r).c_str(),
            index + 1, t ? FormatTable(t->s).c_str() : "not there");
    }
    WriteTables(room);
}

void Keeper::UpdateTables(Room& room, int64_t nowMs) {
    for (int i = 0; i < static_cast<int>(room.tables.size()); i++) {
        Table& t = room.tables[i];
        const int64_t in = nowMs - t.phaseAtMs;
        if (t.s.phase == TablePhase::Call && in >= 1000 * (t.s.callSeconds ? t.s.callSeconds : kCallSeconds)) {
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
        } else if (t.s.phase == TablePhase::Connecting && in >= kConnectMs) {
            Log("keeper: room %s: table %d's match room never opened; calling again", room.code.c_str(), i + 1);
            Fill(room, t, nowMs, true);
        } else if (t.s.phase == TablePhase::After && in >= kAfterMs) {
            Fill(room, t, nowMs);
        }
    }
    WriteTables(room);
}

} // namespace sfv::keeper
