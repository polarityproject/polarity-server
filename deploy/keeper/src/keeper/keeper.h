#pragma once
// sfvr-keeper (docs/online-lobby.md, "sfvr-keeper"; custom-rooms-plan.md M3):
// hosts the presence rooms on the rooms server, one Weyvelength client per
// room. The lobby channel the hub sits in (listed: kind=lobby, build=<lobby
// protocol>, count), where players ask for custom rooms; and the custom rooms
// it creates for them (the requester becomes the owner). It proves itself in
// each room (keeper_protocol.h), answers requests privately over P2P, writes
// the public room list into the lobby, keeps rooms to 16 members, hands a room
// to its longest-present member when the owner leaves, kicks chat floods, and
// closes a room once it is empty.
//
// Single-threaded: Step() polls every client. Portable (Windows for local
// tests, Linux on the server).

#include "core/keeper_protocol.h"
#include "core/spectate_relay.h"
#include "keeper/sign.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

struct WeyveClient;

namespace sfv::keeper {

struct KeeperOptions {
    std::string host = "127.0.0.1";
    uint16_t port = 5555;
    std::string statePath;   // the room codes, to reclaim after a restart; empty: none kept
    KeyPair key;
    uint16_t protocol = 0;   // the lobby protocol the channel is for
    std::string name = "SFVR";
    int maxRooms = 500;
    // The local UDP ports of its P2P links (--ports 47200-47299): the server's
    // firewall opens this range. 0 0: any.
    uint16_t portBegin = 0, portEnd = 0;
    // Every link on one UDP socket (in that range) instead of a port each:
    // a port per link ran out at about 100 links. Off: --per-link-ports.
    bool linkMux = true;
    // The name filter's words (FoldName'd; --words <file>, one a line): a
    // public room shows those names as "Player N".
    std::vector<std::string> words;
    std::function<void(const std::string&)> log; // one line, no newline
};

class Keeper {
public:
    explicit Keeper(KeeperOptions options);
    ~Keeper();
    // Connects and joins (or creates) the lobby channel, and rejoins the
    // custom rooms kept in the state file. False if the server cannot be
    // reached (Step retries).
    bool Start(int64_t nowMs);
    void Step(int64_t nowMs);

    // For tests and status lines.
    std::string LobbyCode() const;
    size_t CustomRooms() const;

    // Observability (main.cpp writes both files every few seconds, --metrics
    // and --status). The keeper's counters and gauges in the Prometheus text
    // format (node_exporter's textfile collector reads it); the longest step
    // since the last call starts over.
    std::string MetricsText();
    // The public room list (as the lobby lists it) and the totals, as JSON
    // for the rooms API (FormatRoomListJson).
    std::string StatusJson(int64_t unixMs) const;

private:
    struct Member {
        uint32_t id = 0;
        std::string name;
        int64_t joinedMs = 0;
        ChatLimiter chat;
        uint32_t lastSeq = 0;      // highest request handled
        int64_t lastRoomAskMs = -1;
        int64_t lastPeekMs = -1;
        int callStrikes = 0; // your-turn calls let lapse in a row (twice: off the queue)
        bool removed = false; // kicked: what they sent before going is not acted on
        std::string chara;         // member data `ch`, for the list's faces
    };
    struct Outgoing {
        uint32_t to = 0;
        uint32_t seq = 0;
        std::string datagram;
        int64_t nextMs = 0;
        int64_t untilMs = 0;
        int sends = 0;
    };
    enum class Kind { Lobby, Custom };
    // A table set's spectator relay (core/spectate_relay.h): the host of a
    // room of its own (`down`) where every viewer of the set watches, and,
    // from its first viewer on, a member of the set's match room (`up`, the
    // host streams to it alone; with nobody watching, P1 streams to nobody).
    // It outlives the set while viewers behind still play its games.
    struct RelayLink {
        int table = -1;
        std::string matchCode;
        std::string relayCode;
        WeyveClient* up = nullptr;
        WeyveClient* down = nullptr;
        bool upIn = false, downIn = false;
        uint32_t hostId = 0;
        bool hostSet = false;
        std::vector<std::pair<std::string, std::string>> data; // the match room's build keys, for the relay room
        bool published = false; // the relay room is the table's `watch`
        std::unique_ptr<SpectateRelay> core;
        int64_t startedMs = 0;
        int64_t upStartedMs = -1; // joined the match room (on the first viewer); -1 not yet
        int64_t statsMs = 0;
        bool ended = false;
    };
    // A table of a custom room (keeper_protocol.h TableState): its state and
    // queue are written to room data; the rest is the keeper's own.
    struct Table {
        TableState s;
        std::vector<uint32_t> queue;
        Rules rules;          // t<n>: the next set's
        Rules setRules;       // the running set's (fixed when it was called)
        uint64_t token = 0;   // the running set's P2 seat token
        std::string build;    // P1's build (its code report), for the relay room
        int64_t phaseAtMs = 0; // when the phase began (its time limit)
        std::string written;  // s<n> and q<n> as last written
    };
    struct Room {
        Kind kind = Kind::Custom;
        WeyveClient* client = nullptr;
        std::string code;      // known once in the room (or kept from before a restart)
        bool inRoom = false;
        bool host = false;
        bool setUp = false;    // host and its data written
        bool joinAsked = false;
        bool lobbyListAsked = false; // the lobby: looking for one of its build to adopt (FindLobby)
        std::vector<uint32_t> order; // members by join time, oldest first
        std::map<uint32_t, Member> members;
        std::deque<Outgoing> outbox;
        std::map<std::string, std::string> written; // room data as we last wrote it
        int64_t createdMs = 0;
        int64_t reconnectMs = 0;
        // Custom rooms.
        RoomSettings settings;
        uint32_t owner = 0;
        bool ownerArrived = false;
        uint32_t requester = 0;  // the lobby member who asked (answered once the code is known)
        uint32_t requestSeq = 0;
        bool reclaimed = false;  // rejoined after a restart: settings come from its room data
        int64_t emptySinceMs = -1;
        bool closing = false;
        std::vector<uint32_t> masked; // members whose names the filter rejected (public rooms)
        std::vector<Table> tables;
        bool unlisted = false; // the operator took it off the public list
        std::vector<std::unique_ptr<RelayLink>> relays;
    };

    void Log(const char* format, ...) const;
    bool Connect(Room& room);
    WeyveClient* NewClient();
    // Relays.
    void StartRelay(Room& room, int table, int64_t nowMs);
    void PumpRelays(Room& room, int64_t nowMs);
    void PumpRelayUp(Room& room, RelayLink& r, int64_t nowMs);
    void PumpRelayDown(Room& room, RelayLink& r, int64_t nowMs);
    void EndRelay(Room& room, RelayLink& r, const char* why);
    void FindLobby(Room& room);
    void OnLobbyList(Room& room);
    void Pump(Room& room, int64_t nowMs);
    void OnInRoom(Room& room, const std::string& code, int64_t nowMs);
    void SetUpAsHost(Room& room, int64_t nowMs);
    void ReadBackCustom(Room& room);
    void OnJoined(Room& room, uint32_t id, const std::string& name, int64_t nowMs);
    void OnLeft(Room& room, uint32_t id, int64_t nowMs);
    void OnMemberData(Room& room, uint32_t id, const std::string& key, const std::string& value, int64_t nowMs);
    void OnChat(Room& room, uint32_t from, const std::string& text, int64_t nowMs);
    void OnRequest(Room& room, Member& m, const Request& request, int64_t nowMs);
    void Answer(Room& room, uint32_t to, const keeper::Answer& answer, int64_t nowMs);
    void SendOutbox(Room& room, int64_t nowMs);
    void WriteData(Room& room, const std::string& key, const std::string& value);
    void DeleteData(Room& room, const std::string& key);
    void UpdateCustom(Room& room, int64_t nowMs);
    void UpdateLobby(Room& room, int64_t nowMs);
    void Close(Room& room, const char* why);
    // The operator's commands: a file beside the state file (<state>.cmd),
    // read and emptied each second: "unlist <code>", "list <code>",
    // "word <word>" (into the name filter), "rooms" (a summary in the log).
    void Commands(int64_t nowMs);
    int64_t commandsAtMs_ = 0;
    void CheckName(Room& room, uint32_t id);
    void WriteMask(Room& room);
    std::string ShownName(const Room& room, uint32_t id) const;
    void Peek(Room& lobby, Member& m, const Request& request, int64_t nowMs);
    // Tables.
    void SetUpTables(Room& room, int64_t nowMs);
    void TableRequest(Room& room, Member& m, const Request& request, int64_t nowMs);
    void UpdateTables(Room& room, int64_t nowMs);
    void SetPhase(Table& t, TablePhase phase, int64_t nowMs);
    // `brokeOff`: the table's set or its handoff ended early; its players
    // get a longer call (their games are leaving the battle).
    void Fill(Room& room, Table& t, int64_t nowMs, bool brokeOff = false);
    void WriteTables(Room& room);
    void OffTables(Room& room, uint32_t id, int64_t nowMs);
    void Ticket(Room& room, uint32_t to, const TableTicket& ticket, int64_t nowMs);
    // A word to a player unasked (a ticket, the again answer to both): sent
    // until acknowledged.
    void Notice(Room& room, uint32_t to, const std::string& kind, bool ok, const std::string& text, int64_t nowMs);
    std::mt19937_64 rng_{std::random_device{}()};
    // The rooms the lobby lists (public, set up, someone in them, not unlisted).
    std::vector<ListedRoom> PublicList() const;
    // Counters since the start, for MetricsText.
    struct Stats {
        static constexpr int kBuckets = 10;
        static constexpr double kBucketSeconds[kBuckets] = {0.001, 0.002, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0};
        uint64_t steps = 0;
        double stepSeconds = 0;
        uint64_t stepBuckets[kBuckets] = {}; // steps at or under each limit (not cumulative)
        double stepMaxSeconds = 0;           // since the last MetricsText
        std::map<std::string, uint64_t> requests; // by kind (known kinds; the rest "other")
        uint64_t answers = 0, resends = 0, notices = 0, tickets = 0, calls = 0, callsLapsed = 0;
        std::map<std::string, uint64_t> sets; // how sets ended: result, forfeit, again, unfinished
        uint64_t roomsAsked = 0, roomsClosed = 0, floodKicks = 0, ownerKicks = 0, serverLost = 0, relays = 0;
        uint64_t connects = 0, connectFailures = 0;
        double connectSeconds = 0, connectMaxSeconds = 0; // the max since the last MetricsText
    } stats_;
    void SaveState();
    std::vector<std::string> LoadState();
    int Count(const Room& room) const { return static_cast<int>(room.members.size()); }

    KeeperOptions o_;
    std::unique_ptr<Room> lobby_;
    std::vector<std::unique_ptr<Room>> rooms_;
    int64_t now_ = 0;
    int64_t listWrittenMs_ = -1;
    int64_t countWrittenMs_ = -1;
    std::string stateWritten_;
};

} // namespace sfv::keeper
