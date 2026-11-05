#include "core/spectate_relay.h"

#include <algorithm>
#include <cstdio>

namespace sfv {

namespace {

constexpr int64_t kHelloEveryMs = 1000;
constexpr int64_t kLoadedEveryMs = 500;
constexpr int64_t kAckEveryMs = 100;
constexpr int64_t kBeatMs = 250;
constexpr int64_t kListEveryMs = 1000;
constexpr int64_t kStreamResendMs = 300;
constexpr int64_t kWelcomeAfterMs = 1500; // after a viewer's Accept (its list first)
constexpr int kBatchesAhead = 4; // as the host's (netplay_driver.cpp kStreamBatchesAhead)
constexpr int64_t kViewerSilenceMs = 10000;
constexpr int64_t kHostSilenceMs = 15000;
// After the host has left: how long viewers far behind are still served.
constexpr int64_t kKeepAfterHostMs = 10 * 60 * 1000;
constexpr int64_t kCloseAfterHostMs = 3000;
// Region requests to the host: a batch at most this often, of at most this
// many regions; a region asked for again only after kAskAgainMs.
constexpr int64_t kAskEveryMs = 250;
constexpr size_t kRegionsPerAsk = 64;
constexpr int64_t kAskAgainMs = 1500;
constexpr size_t kMaxGames = lobby::kKeptMatches;
constexpr uint32_t kMaxRegionBytes = 8u << 20;
constexpr uint32_t kMaxFrames = 1u << 20;
// The relay's Loaded checksum: never the host's (a host whose state folds to
// exactly 0 skips the hashes; so does one with reconcile_start off), so the
// host sends its region hashes. Without them for kHashesWaitMs the relay
// streams anyway, and a viewer whose state differs starts without them, as
// it would from such a host.
constexpr uint32_t kNoChecksum = 0;
constexpr int64_t kHashesWaitMs = 3000;

uint32_t Now32(int64_t ms) { return static_cast<uint32_t>(ms); }

} // namespace

SpectateRelay::SpectateRelay(RelayConfig config, Send toHost, Send toViewer)
    : config_(std::move(config)), toHost_(std::move(toHost)), toViewer_(std::move(toViewer))
{
}

void SpectateRelay::Log(const std::string& line) const
{
    if (config_.log) config_.log(line);
}

void SpectateRelay::Up(const lobby::Message& m)
{
    if (host_ && !hostLeft_) toHost_(host_, m);
}

void SpectateRelay::Down(uint64_t to, const lobby::Message& m)
{
    toViewer_(to, m);
    stats_.bytesToViewers += lobby::Encode(m).size();
}

SpectateRelay::Game& SpectateRelay::GameOf(uint64_t id)
{
    auto it = games_.find(id);
    if (it != games_.end()) return it->second;
    Game& g = games_[id];
    g.id = id;
    order_.push_back(id);
    while (order_.size() > kMaxGames) {
        games_.erase(order_.front());
        order_.erase(order_.begin());
    }
    return games_[id];
}

SpectateRelay::Game* SpectateRelay::Find(uint64_t id)
{
    auto it = games_.find(id);
    return it == games_.end() ? nullptr : &it->second;
}

const SpectateRelay::Game* SpectateRelay::Find(uint64_t id) const
{
    auto it = games_.find(id);
    return it == games_.end() ? nullptr : &it->second;
}

bool SpectateRelay::Complete(const Game& g) const
{
    return g.listed && g.entry.finished && g.frames.size() >= g.entry.frames && (g.hashesComplete || g.noHashes) && g.hasLoaded;
}

int SpectateRelay::IndexOf(uint64_t id) const
{
    const auto it = std::find(order_.begin(), order_.end(), id);
    return it == order_.end() ? -1 : static_cast<int>(it - order_.begin());
}

void SpectateRelay::SetHost(uint64_t id, uint64_t build)
{
    host_ = id;
    build_ = build;
    hostHeardMs_ = lastStepMs_;
}

void SpectateRelay::HostGone(int64_t nowMs)
{
    if (hostLeft_) return;
    hostLeft_ = true;
    hostGoneMs_ = nowMs;
    Log("relay: the host is gone; " + std::to_string(order_.size()) + " games kept for " + std::to_string(viewers_.size()) +
        " viewers");
}

// ----- the host ----------------------------------------------------------------

void SpectateRelay::FromHost(const lobby::Message& m, int64_t nowMs)
{
    if (hostLeft_) return;
    hostHeardMs_ = nowMs;
    stats_.bytesFromHost += lobby::Encode(m).size();
    switch (m.type) {
    case lobby::Type::Accept:
        if (!accepted_) Log("relay: watching " + m.name + "'s set");
        accepted_ = true;
        break;
    case lobby::Type::Reject:
        // The host does not know this member is the keeper yet (its proof
        // arrives through the server): Hello again in a moment.
        if (!accepted_) Log("relay: the host turned the relay away (" + std::to_string(static_cast<int>(m.reason)) + "); trying again");
        helloMs_ = nowMs + 1000;
        break;
    case lobby::Type::SessionState:
        state_ = m;
        haveState_ = true;
        if (m.matchId) live_ = m.matchId;
        CheckDecided();
        break;
    case lobby::Type::Welcome: {
        Game& g = GameOf(m.matchId);
        if (!g.hasWelcome) Log("relay: game " + std::to_string(IndexOf(m.matchId) + 1) + " announced");
        g.welcome = m;
        g.hasWelcome = true;
        live_ = m.matchId;
        break;
    }
    case lobby::Type::Loaded:
        if (Game* g = Find(m.matchId)) {
            g->loaded = m;
            g->hasLoaded = true;
        }
        break;
    case lobby::Type::StateHashes: {
        Game* g = Find(m.matchId);
        if (!g || g->hashesComplete || m.total == 0 || m.total > 100000) break;
        if (g->hashes.size() != m.total) {
            g->hashes.assign(m.total, 0);
            g->hashParts.assign((m.total + lobby::kHashesPerMessage - 1) / lobby::kHashesPerMessage, false);
        }
        if (m.first % lobby::kHashesPerMessage || m.first + m.hashes.size() > m.total) break;
        std::copy(m.hashes.begin(), m.hashes.end(), g->hashes.begin() + m.first);
        g->hashParts[m.first / lobby::kHashesPerMessage] = true;
        g->hashesComplete = std::all_of(g->hashParts.begin(), g->hashParts.end(), [](bool b) { return b; });
        break;
    }
    case lobby::Type::StateData:
        if (Game* g = Find(m.matchId)) AddStateData(*g, m);
        break;
    case lobby::Type::Inputs:
        if (Game* g = Find(m.matchId)) AddFrames(*g, m);
        break;
    case lobby::Type::Matches:
        OnHostList(m);
        break;
    case lobby::Type::Bye:
        byeText_ = m.text;
        byeForfeit_ = m.forfeit;
        Log("relay: the host left the session" + (m.text.empty() ? std::string() : ": " + m.text));
        HostGone(nowMs);
        break;
    default:
        break;
    }
}

void SpectateRelay::OnHostList(const lobby::Message& m)
{
    if (hostSlices_.size() != m.sliceOf) hostSlices_.assign(m.sliceOf, lobby::MatchEntry{});
    for (size_t i = 0; i < m.matches.size() && m.sliceAt + i < hostSlices_.size(); i++) hostSlices_[m.sliceAt + i] = m.matches[i];
    if (!std::all_of(hostSlices_.begin(), hostSlices_.end(), [](const lobby::MatchEntry& e) { return e.id != 0; })) return;
    // The host's order. Games it no longer lists (older ones it dropped) stay
    // in front, in their order; one known only from its Welcome so far (the
    // next game, before the host's list has it) stays after: viewers take
    // the next game by its place in the list (it once came first, and live
    // viewers loaded game 1 again for game 2).
    std::vector<uint64_t> listed, front, back;
    for (const lobby::MatchEntry& e : hostSlices_) {
        Game& g = GameOf(e.id);
        g.listed = true;
        g.entry = e;
        listed.push_back(e.id);
    }
    bool pastListed = false;
    for (uint64_t id : order_) {
        if (std::find(listed.begin(), listed.end(), id) != listed.end()) pastListed = true;
        else (pastListed ? back : front).push_back(id);
    }
    std::vector<uint64_t> order = front;
    order.insert(order.end(), listed.begin(), listed.end());
    order.insert(order.end(), back.begin(), back.end());
    order_ = order;
    while (order_.size() > kMaxGames) {
        games_.erase(order_.front());
        order_.erase(order_.begin());
    }
}

void SpectateRelay::AddFrames(Game& g, const lobby::Message& m)
{
    for (size_t i = 0; i < m.frames.size(); i++) {
        const uint32_t at = m.first + static_cast<uint32_t>(i);
        if (at >= kMaxFrames) break;
        if (at < g.frames.size()) continue;
        if (at == g.frames.size()) g.frames.push_back(m.frames[i]);
        else g.ahead[at] = m.frames[i];
    }
    for (auto it = g.ahead.begin(); it != g.ahead.end() && it->first <= g.frames.size();) {
        if (it->first == g.frames.size()) g.frames.push_back(it->second);
        it = g.ahead.erase(it);
    }
}

void SpectateRelay::AddStateData(Game& g, const lobby::Message& m)
{
    if (m.total > kMaxRegionBytes || m.offset % lobby::kStateChunk || m.offset + m.bytes.size() > m.total) return;
    Region& r = g.regions[m.region];
    if (r.complete) return;
    if (r.total != m.total || r.bytes.size() != m.total) {
        r.total = m.total;
        r.bytes.assign(m.total, 0);
        r.chunks.assign((m.total + lobby::kStateChunk - 1) / lobby::kStateChunk, false);
    }
    std::copy(m.bytes.begin(), m.bytes.end(), r.bytes.begin() + m.offset);
    if (!r.chunks.empty()) r.chunks[m.offset / lobby::kStateChunk] = true;
    r.complete = std::all_of(r.chunks.begin(), r.chunks.end(), [](bool b) { return b; });
    stats_.stateBytesFromHost += m.bytes.size();
    if (r.complete) g.want.erase(m.region);
}

void SpectateRelay::CheckDecided()
{
    if (decided_ || config_.firstTo <= 0 || !haveState_) return;
    if (state_.seats[0].wins >= config_.firstTo || state_.seats[1].wins >= config_.firstTo) {
        decided_ = true;
        Log("relay: the set is decided");
    }
}

void SpectateRelay::StepHost(int64_t nowMs)
{
    if (!host_ || hostLeft_) return;
    if (accepted_ && nowMs - hostHeardMs_ > kHostSilenceMs) {
        Log("relay: the host is silent");
        HostGone(nowMs);
        return;
    }
    if (!accepted_) {
        if (nowMs - helloMs_ >= kHelloEveryMs) {
            helloMs_ = nowMs;
            lobby::Message hello;
            hello.type = lobby::Type::Hello;
            hello.build = build_;
            hello.role = lobby::Role::Spectator;
            hello.name = config_.name;
            Up(hello);
        }
        return;
    }
    // The game to take: the oldest one the host has that is not in hand yet.
    const Game* up = Find(upMatch_);
    if (!up || Complete(*up)) {
        uint64_t next = 0;
        for (uint64_t id : order_) {
            const Game& g = games_.at(id);
            if (!Complete(g) && (g.listed || id == live_)) {
                next = id;
                break;
            }
        }
        if (next != upMatch_) {
            upMatch_ = next;
            upStreaming_ = false;
            upLoadedMs_ = -1000000;
            upSinceMs_ = nowMs;
        }
    }
    if (Game* g = Find(upMatch_)) {
        if (!upStreaming_) {
            if (g->hasLoaded && !g->hashesComplete && !g->noHashes && nowMs - upSinceMs_ > kHashesWaitMs) {
                g->noHashes = true;
                Log("relay: no region hashes from the host for game " + std::to_string(IndexOf(g->id) + 1) + "; streaming without");
            }
            if ((g->hashesComplete || g->noHashes) && g->hasLoaded) {
                lobby::Message done;
                done.type = lobby::Type::Loaded;
                done.matchId = g->id;
                done.setup = g->loaded.setup;
                done.stateChecksum = g->loaded.stateChecksum;
                done.reconciled = true;
                Up(done);
                upStreaming_ = true;
                upAckMs_ = -1000000;
            }
            else if (nowMs - upLoadedMs_ >= kLoadedEveryMs) {
                upLoadedMs_ = nowMs;
                lobby::Message loaded;
                loaded.type = lobby::Type::Loaded;
                loaded.matchId = g->id;
                loaded.setup = g->hasWelcome ? g->welcome.setup : g->entry.setup;
                loaded.stateChecksum = kNoChecksum;
                loaded.reconciled = false;
                Up(loaded);
            }
        }
        if (upStreaming_ && nowMs - upAckMs_ >= kAckEveryMs) {
            upAckMs_ = nowMs;
            lobby::Message ack;
            ack.type = lobby::Type::InputsAck;
            ack.matchId = g->id;
            ack.first = static_cast<uint32_t>(g->frames.size());
            ack.time = Now32(nowMs);
            Up(ack);
        }
    }
    else if (nowMs - upAckMs_ >= kBeatMs) {
        // All in hand: a heartbeat (matchId 0: not streaming anything).
        upAckMs_ = nowMs;
        lobby::Message ack;
        ack.type = lobby::Type::InputsAck;
        ack.time = Now32(nowMs);
        Up(ack);
    }

    // Starting-state regions: what viewers asked for first, then, once the
    // set is decided, the whole state of each game a viewer has not reached
    // (the host is on its results screen; after it leaves nobody has them).
    Game* ask = nullptr;
    for (uint64_t id : order_) {
        Game& g = games_.at(id);
        if (!g.want.empty()) {
            ask = &g;
            break;
        }
    }
    if (!ask && decided_) {
        for (size_t i = 0; i < order_.size() && !ask; i++) {
            Game& g = games_.at(order_[i]);
            if (!g.hashesComplete) continue;
            bool behind = false;
            for (const auto& [id, v] : viewers_) behind |= IndexOf(v.match) < static_cast<int>(i);
            if (!behind) continue;
            for (uint32_t r = 0; r < g.hashes.size(); r++) {
                const auto it = g.regions.find(r);
                if (it == g.regions.end() || !it->second.complete) g.want.insert(r);
            }
            if (!g.want.empty()) {
                Log("relay: taking game " + std::to_string(i + 1) + "'s starting state (" + std::to_string(g.want.size()) +
                    " regions) for viewers behind");
                ask = &g;
            }
        }
    }
    if (ask && nowMs - ask->askedMs >= kAskEveryMs) {
        lobby::Message req;
        req.type = lobby::Type::StateRequest;
        req.matchId = ask->id;
        for (uint32_t r : ask->want) {
            // A region asked for is on its way: again only after a while.
            const auto at = ask->askedAt.find(r);
            if (at != ask->askedAt.end() && nowMs - at->second < kAskAgainMs) continue;
            req.regions.push_back(r);
            if (req.regions.size() >= kRegionsPerAsk) break;
        }
        if (!req.regions.empty()) {
            ask->askedMs = nowMs;
            for (uint32_t r : req.regions) ask->askedAt[r] = nowMs;
            Up(req);
        }
    }
}

// ----- the viewers ----------------------------------------------------------------

void SpectateRelay::FromViewer(uint64_t id, const lobby::Message& m, int64_t nowMs)
{
    auto it = viewers_.find(id);
    if (m.type == lobby::Type::Hello) {
        lobby::Message out;
        if (m.protocol != lobby::kProtocol) {
            out.type = lobby::Type::Reject;
            out.reason = lobby::RejectReason::ProtocolMismatch;
            Down(id, out);
            return;
        }
        if (build_ && m.build != build_) {
            out.type = lobby::Type::Reject;
            out.reason = lobby::RejectReason::BuildMismatch;
            Down(id, out);
            return;
        }
        if (it == viewers_.end()) {
            if (viewers_.size() >= config_.maxViewers || hostLeft_) {
                out.type = lobby::Type::Reject;
                out.reason = lobby::RejectReason::Busy;
                Down(id, out);
                return;
            }
            Viewer& v = viewers_[id];
            v.name = m.name;
            v.heardMs = nowMs;
            v.acceptedMs = nowMs;
            Log("relay: " + m.name + " watches (" + std::to_string(viewers_.size()) + " viewers)");
            it = viewers_.find(id);
        }
        out.type = lobby::Type::Accept;
        out.role = lobby::Role::Spectator;
        out.name = haveState_ ? state_.seats[0].name : std::string();
        Down(id, out);
        // The set's games at once: a table viewer starts at the first, and
        // with the live game's Welcome first it loaded that one, then the
        // first (half a minute lost).
        SendList(nowMs, id);
        return;
    }
    if (it == viewers_.end()) return;
    Viewer& v = it->second;
    v.heardMs = nowMs;
    switch (m.type) {
    case lobby::Type::Loaded:
        if (!Find(m.matchId)) break;
        if (v.match != m.matchId) {
            v.streaming = false;
            v.wanted = 0;
            v.sentThrough = -1;
        }
        if (v.match != m.matchId) v.decided = false;
        v.match = m.matchId;
        v.loaded = true;
        v.checksum = m.stateChecksum;
        v.reconciled = m.reconciled;
        if (const Game* g = Find(m.matchId); g && g->hasLoaded) Down(id, g->loaded);
        break;
    case lobby::Type::StateRequest: {
        Game* g = Find(m.matchId);
        if (!g) break;
        std::vector<uint32_t> have;
        for (uint32_t r : m.regions) {
            const auto reg = g->regions.find(r);
            if (reg != g->regions.end() && reg->second.complete) have.push_back(r);
            else if (r < g->hashes.size() || g->hashes.empty()) g->want.insert(r);
        }
        SendRegions(id, *g, have);
        break;
    }
    case lobby::Type::InputsAck:
        v.ackMatch = m.matchId;
        v.ackFirst = m.first;
        v.played = m.playedTo;
        v.decided = m.decided;
        v.heardTime = m.time;
        v.heardTimeAtMs = nowMs;
        if (!m.matchId) {
            v.streaming = false;
        }
        else if (Find(m.matchId)) {
            if (!v.streaming || v.match != m.matchId) {
                v.streaming = true;
                v.match = m.matchId;
                v.sentThrough = static_cast<int>(m.first) - 1;
                v.progressMs = nowMs;
            }
            if (static_cast<int>(m.first) > v.wanted) {
                v.wanted = static_cast<int>(m.first);
                v.progressMs = nowMs;
            }
        }
        break;
    case lobby::Type::Bye:
        Log("relay: " + v.name + " stopped watching");
        viewers_.erase(it);
        break;
    default:
        break;
    }
}

void SpectateRelay::ViewerGone(uint64_t id)
{
    if (viewers_.erase(id)) Log("relay: a viewer left (" + std::to_string(viewers_.size()) + " watching)");
}

void SpectateRelay::SendRegions(uint64_t to, const Game& g, const std::vector<uint32_t>& regions)
{
    for (uint32_t index : regions) {
        const Region& r = g.regions.at(index);
        for (size_t at = 0; at < r.total || (r.total == 0 && at == 0); at += lobby::kStateChunk) {
            lobby::Message d;
            d.type = lobby::Type::StateData;
            d.matchId = g.id;
            d.region = index;
            d.total = r.total;
            d.offset = static_cast<uint32_t>(at);
            const size_t n = std::min(lobby::kStateChunk, static_cast<size_t>(r.total) - at);
            d.bytes.assign(r.bytes.begin() + static_cast<std::ptrdiff_t>(at), r.bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
            Down(to, d);
            if (r.total == 0) break;
        }
    }
}

const SpectateRelay::Game* SpectateRelay::LastPlayed() const
{
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const Game& g = games_.at(*it);
        if (!g.frames.empty()) return &g;
    }
    return nullptr;
}

bool SpectateRelay::ViewerDone(const Viewer& v) const
{
    // The host gone, nothing more comes: done once on the last game played
    // and its battle at the result, or played through every frame there is
    // (a set that broke off mid-game). `first` alone is not it: a viewer
    // takes the frames far ahead of what it shows.
    if (!hostLeft_) return false;
    const Game* last = LastPlayed();
    if (!last) return true;
    if (v.match != last->id || v.ackMatch != last->id) return false;
    return v.decided || v.played >= last->frames.size();
}

void SpectateRelay::SendList(int64_t nowMs, uint64_t only)
{
    (void)nowMs;
    std::vector<lobby::MatchEntry> list;
    for (uint64_t id : order_) {
        const Game& g = games_.at(id);
        if (!g.hasWelcome && !g.listed) continue;
        lobby::MatchEntry e;
        e.id = id;
        e.setup = g.hasWelcome ? g.welcome.setup : g.entry.setup;
        e.frames = static_cast<uint32_t>(g.frames.size());
        e.finished = Complete(g) || (hostLeft_ && g.listed && g.entry.finished);
        e.winner = g.entry.winner;
        list.push_back(e);
    }
    const size_t total = list.size();
    for (size_t at = 0; at < std::max<size_t>(total, 1); at += lobby::kMatchesPerMessage) {
        lobby::Message slice;
        slice.type = lobby::Type::Matches;
        slice.sliceAt = static_cast<uint8_t>(at);
        slice.sliceOf = static_cast<uint8_t>(total);
        const size_t end = std::min(total, at + lobby::kMatchesPerMessage);
        slice.matches.assign(list.begin() + static_cast<std::ptrdiff_t>(std::min(at, total)),
                             list.begin() + static_cast<std::ptrdiff_t>(end));
        for (const auto& [id, v] : viewers_) {
            if (!only || id == only) Down(id, slice);
        }
    }
}

void SpectateRelay::Stream(int64_t nowMs)
{
    for (auto& [id, v] : viewers_) {
        if (!v.streaming) continue;
        const Game* g = Find(v.match);
        if (!g) continue;
        const int have = static_cast<int>(g->frames.size());
        if (nowMs - v.progressMs > kStreamResendMs && v.sentThrough >= v.wanted) {
            v.sentThrough = v.wanted - 1;
            v.progressMs = nowMs;
        }
        const int limit = v.wanted + kBatchesAhead * static_cast<int>(lobby::kInputsPerMessage);
        while (v.sentThrough + 1 < have && v.sentThrough + 1 < limit) {
            lobby::Message m;
            m.type = lobby::Type::Inputs;
            m.matchId = g->id;
            m.first = static_cast<uint32_t>(v.sentThrough + 1);
            const int end = std::min({have, limit, v.sentThrough + 1 + static_cast<int>(lobby::kInputsPerMessage)});
            m.frames.assign(g->frames.begin() + (v.sentThrough + 1), g->frames.begin() + end);
            v.sentThrough = end - 1;
            Down(id, m);
        }
    }
}

void SpectateRelay::StepViewers(int64_t nowMs)
{
    for (auto it = viewers_.begin(); it != viewers_.end();) {
        if (nowMs - it->second.heardMs > kViewerSilenceMs) {
            Log("relay: " + it->second.name + " silent; dropped");
            it = viewers_.erase(it);
        }
        else {
            ++it;
        }
    }
    Stream(nowMs);
    if (nowMs - beatMs_ < kBeatMs) return;
    beatMs_ = nowMs;
    const Game* live = hostLeft_ ? LastPlayed() : Find(live_);
    const bool lobbyPhase = haveState_ && !hostLeft_ && state_.phase == lobby::SessionPhase::Lobby;
    std::vector<uint64_t> finished;
    for (auto& [id, v] : viewers_) {
        if (haveState_) {
            lobby::Message st = state_;
            st.spectators = static_cast<uint8_t>(std::min<size_t>(viewers_.size(), 255));
            if (hostLeft_) st.phase = lobby::SessionPhase::PostMatch;
            // A player chose character select: only a viewer at the live
            // game's result goes to the select screen with them (and stays
            // there while they are on it). One still playing a game, the
            // live one or an earlier one, is told the match goes on: the
            // select phase took it out of its game, which it lost the rest of.
            if (st.phase == lobby::SessionPhase::Lobby) {
                if (live && v.match == live->id && v.decided) v.inSelect = true;
                if (!v.inSelect && v.match) st.phase = lobby::SessionPhase::Match;
            }
            else {
                v.inSelect = false;
            }
            st.time = Now32(nowMs);
            st.echo = v.heardTime;
            st.echoAge = static_cast<uint16_t>(std::clamp<int64_t>(nowMs - v.heardTimeAtMs, 0, 65535));
            Down(id, st);
        }
        // The players' match: Welcome to whoever is not on it (a viewer still
        // on an earlier game takes it up once that one has ended).
        // Not in a new viewer's first moments: the list comes first (Accept).
        if (live && live->hasWelcome && !lobbyPhase && v.match != live->id && nowMs - v.acceptedMs >= kWelcomeAfterMs)
            Down(id, live->welcome);
        if (const Game* g = Find(v.match); g && !v.streaming) {
            if (g->hasLoaded) Down(id, g->loaded);
            // A state that differs: the hashes, until it says it has reconciled.
            if (v.loaded && !v.reconciled && g->hasLoaded && g->hashesComplete && v.checksum != g->loaded.stateChecksum) {
                for (size_t first = 0; first < g->hashes.size(); first += lobby::kHashesPerMessage) {
                    lobby::Message h;
                    h.type = lobby::Type::StateHashes;
                    h.matchId = g->id;
                    h.first = static_cast<uint32_t>(first);
                    h.total = static_cast<uint32_t>(g->hashes.size());
                    const size_t n = std::min(lobby::kHashesPerMessage, g->hashes.size() - first);
                    h.hashes.assign(g->hashes.begin() + static_cast<std::ptrdiff_t>(first),
                                    g->hashes.begin() + static_cast<std::ptrdiff_t>(first + n));
                    Down(id, h);
                }
            }
        }
        if (ViewerDone(v) || (hostLeft_ && nowMs - hostGoneMs_ > kKeepAfterHostMs)) {
            if (v.byes < 3 && nowMs - v.byeMs >= 500) {
                lobby::Message bye;
                bye.type = lobby::Type::Bye;
                bye.text = byeText_;
                bye.forfeit = byeForfeit_;
                Down(id, bye);
                v.byes++;
                v.byeMs = nowMs;
                if (v.byes == 1) Log("relay: " + v.name + " has seen the set; Bye");
            }
            if (v.byes >= 3) finished.push_back(id);
        }
    }
    for (uint64_t id : finished) viewers_.erase(id);
    if (nowMs - listMs_ >= kListEveryMs) {
        listMs_ = nowMs;
        SendList(nowMs);
    }
}

void SpectateRelay::Step(int64_t nowMs)
{
    lastStepMs_ = nowMs;
    StepHost(nowMs);
    StepViewers(nowMs);
}

bool SpectateRelay::Done(int64_t nowMs) const
{
    if (!hostLeft_) return false;
    if (viewers_.empty() && nowMs - hostGoneMs_ >= kCloseAfterHostMs) return true;
    return nowMs - hostGoneMs_ > kKeepAfterHostMs + 5000;
}

std::string SpectateRelay::ViewersLine() const
{
    std::string out;
    for (const auto& [id, v] : viewers_) {
        if (!out.empty()) out += ", ";
        out += v.name + " g" + std::to_string(IndexOf(v.match) + 1) + (v.streaming ? "s" : "") + (v.decided ? "d" : "") + " " +
               std::to_string(v.played) + "/" + std::to_string(v.ackFirst);
    }
    return out;
}

RelayStats SpectateRelay::Stats() const
{
    RelayStats s = stats_;
    s.viewers = viewers_.size();
    s.games = order_.size();
    s.hostAccepted = accepted_;
    s.hostLeft = hostLeft_;
    s.decided = decided_;
    return s;
}

} // namespace sfv
