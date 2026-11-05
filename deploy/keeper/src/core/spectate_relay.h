#pragma once
// The keeper's spectator relay (custom-rooms-plan.md M5): in a table set the
// host streams to one spectator only, the keeper, and the keeper serves every
// real viewer from a room of its own, so neither player uploads more however
// many watch (user, 2026-11-05).
//
// Toward the host the relay is a spectator that has no game: it says Hello,
// and for each game it sends a Loaded whose checksum cannot match, so the
// host sends that game's region hashes (4 KB, lobby.h's reconcile); it then
// says it has reconciled and takes the input stream as fast as the host
// sends it. It never takes a game's whole starting state (about 0.5 MB, a
// cost to P1's upload) except for the regions a viewer lacks, on demand, and
// once the set is decided, the games a viewer has not reached yet, so it can
// serve them after the host has left.
//
// Toward the viewers the relay is the host as they know it: Accept, the
// host's SessionState (stamped for each viewer), Welcome, the host's Loaded,
// the region hashes and bytes, the match list and the input streams with
// the same window and resends as the host's. A viewer far behind plays every
// game of the set: the relay keeps them after the host leaves, and says Bye
// once the viewer has seen the last one.
//
// No game, no transport: the keeper feeds it decoded messages and sends what
// it hands back (tests drive it the same way).

#include "core/lobby.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sfv {

struct RelayConfig {
    std::string name = "SFVR keeper"; // its name in the host's session
    size_t maxViewers = 16;
    int firstTo = 0; // the set's rules: decided once a player has this many wins (0: never by score)
    std::function<void(const std::string&)> log;
};

struct RelayStats {
    size_t viewers = 0;
    size_t games = 0;
    uint64_t bytesFromHost = 0; // payload bytes of the host's messages
    uint64_t stateBytesFromHost = 0;
    uint64_t bytesToViewers = 0;
    bool hostAccepted = false;
    bool hostLeft = false;
    bool decided = false;
};

class SpectateRelay {
public:
    using Send = std::function<void(uint64_t to, const lobby::Message&)>;
    SpectateRelay(RelayConfig config, Send toHost, Send toViewer);

    // The host: its member id and its build (the match room's `build`, which
    // viewers must have too). Hello goes out from the next Step.
    void SetHost(uint64_t id, uint64_t build);
    // The build viewers must have, before the host is known (its code report).
    void SetBuild(uint64_t build) { build_ = build; }
    // The same two play on (keeper again): the next set's target.
    void SetFirstTo(int firstTo) { config_.firstTo = firstTo; }
    void FromHost(const lobby::Message& m, int64_t nowMs);
    // The host left the match room (or its link is gone).
    void HostGone(int64_t nowMs);

    void FromViewer(uint64_t id, const lobby::Message& m, int64_t nowMs);
    void ViewerGone(uint64_t id);

    // Often (every few ms to 50 ms): Hello and acks, heartbeats, the streams.
    void Step(int64_t nowMs);

    // Set over and the host gone, and every viewer done (or the time limit):
    // the relay can close.
    bool Done(int64_t nowMs) const;
    RelayStats Stats() const;
    // Each viewer: name, game (1-based; 0 none), s streaming, d at the result,
    // played/in hand. For the keeper's log.
    std::string ViewersLine() const;

private:
    struct Region {
        uint32_t total = 0;
        std::vector<uint8_t> bytes;
        std::vector<bool> chunks;
        bool complete = false;
    };
    struct Game {
        uint64_t id = 0;
        bool hasWelcome = false;
        lobby::Message welcome;
        bool hasLoaded = false;
        lobby::Message loaded; // the host's: its setup and checksum
        std::vector<uint64_t> hashes;
        std::vector<bool> hashParts;
        bool hashesComplete = false;
        bool noHashes = false; // the host sent none: streamed without
        std::vector<lobby::FrameRecord> frames;  // [0, frames.size()) in hand
        std::map<uint32_t, lobby::FrameRecord> ahead; // beyond, arrived out of order
        bool listed = false; // in the host's match list
        lobby::MatchEntry entry; // as the host's list last said
        std::map<uint32_t, Region> regions;
        std::set<uint32_t> want; // regions to ask the host for
        int64_t askedMs = -1000000;            // the last request for its regions
        std::map<uint32_t, int64_t> askedAt;   // per region
    };
    struct Viewer {
        std::string name;
        int64_t acceptedMs = 0;
        int64_t heardMs = 0;
        uint32_t heardTime = 0;
        int64_t heardTimeAtMs = 0;
        uint64_t match = 0;
        bool loaded = false;
        uint32_t checksum = 0;
        bool reconciled = false;
        bool streaming = false;
        int wanted = 0;
        int sentThrough = -1;
        int64_t progressMs = 0;
        uint64_t ackMatch = 0;
        uint32_t ackFirst = 0;
        uint32_t played = 0;  // its playback (InputsAck.played)
        bool decided = false; // its battle has reached the result
        bool inSelect = false; // shown the players' select screen (until they leave it)
        int byes = 0;
        int64_t byeMs = 0;
    };

    void Log(const std::string& line) const;
    void Up(const lobby::Message& m);
    void Down(uint64_t to, const lobby::Message& m);
    Game& GameOf(uint64_t id);
    Game* Find(uint64_t id);
    const Game* Find(uint64_t id) const;
    bool Complete(const Game& g) const;
    int IndexOf(uint64_t id) const;
    void OnHostList(const lobby::Message& m);
    void AddFrames(Game& g, const lobby::Message& m);
    void AddStateData(Game& g, const lobby::Message& m);
    void StepHost(int64_t nowMs);
    void StepViewers(int64_t nowMs);
    void Stream(int64_t nowMs);
    void SendRegions(uint64_t to, const Game& g, const std::vector<uint32_t>& regions);
    void SendList(int64_t nowMs, uint64_t only = 0); // only: to that viewer alone
    bool ViewerDone(const Viewer& v) const;
    const Game* LastPlayed() const;
    void CheckDecided();

    RelayConfig config_;
    Send toHost_, toViewer_;

    // The host.
    uint64_t host_ = 0;
    uint64_t build_ = 0;
    bool accepted_ = false;
    int64_t helloMs_ = -1000000;
    int64_t hostHeardMs_ = 0;
    bool hostLeft_ = false;
    int64_t hostGoneMs_ = 0;
    std::string byeText_;
    bool byeForfeit_ = false;
    bool haveState_ = false;
    lobby::Message state_; // the host's last SessionState
    uint64_t live_ = 0;     // the newest match the host announced
    uint64_t upMatch_ = 0;  // the game being taken from the host
    bool upStreaming_ = false;
    int64_t upLoadedMs_ = -1000000;
    int64_t upAckMs_ = 0;
    int64_t upSinceMs_ = 0; // when upMatch_ became the game to take
    bool decided_ = false;
    std::vector<lobby::MatchEntry> hostSlices_;

    std::vector<uint64_t> order_; // games, oldest first
    std::map<uint64_t, Game> games_;
    std::map<uint64_t, Viewer> viewers_;
    int64_t beatMs_ = 0;
    int64_t listMs_ = 0;
    int64_t lastStepMs_ = 0;
    RelayStats stats_;
};

} // namespace sfv
