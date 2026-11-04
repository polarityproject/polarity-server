#pragma once
// What the keeper (src/keeper/, sfvr-keeper on the server) and the mod say to
// each other (docs/online-lobby.md, "sfvr-keeper"; custom-rooms-plan.md M3).
// Portable: the keeper builds on Linux too.
//
// Presence rooms are Weyvelength rooms the keeper hosts: the lobby channel the
// hub sits in, and the custom rooms. Through them:
//
//   - The keeper proves itself in its own member data, `kp`: the base64 of an
//     ECDSA P-256 signature over ProofMessage(room code, its member id), with
//     a key whose public half ships in the mod. Any member can check it, also
//     while another member holds host (after a keeper restart): a member who
//     is host and sees the proven keeper hands host back.
//   - A player asks through its own member data, `rq` = FormatRequest(...),
//     with a sequence number that only grows. The keeper answers privately
//     over a P2P datagram (FormatAnswer) and sends it again until the player
//     writes `ak` = that sequence number.
//   - The keeper writes the room's state into room data (owner, tables, the
//     public room list), which every member mirrors; a keeper that restarts
//     reads it back.

#include "core/rules.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sfv::keeper {

// --- keys ---
// Listing slots and room data of a presence room.
constexpr const char* kKind = "kind";     // "lobby" or "custom"
constexpr const char* kKindLobby = "lobby";
constexpr const char* kKindCustom = "custom";
constexpr const char* kBuild = "build";   // the lobby protocol (lobby::kProtocol)
constexpr const char* kCount = "count";   // members, the keeper not counted (lobby listing)
// Custom room data.
constexpr const char* kOwner = "owner";   // the owner's member id
constexpr const char* kPreset = "name";   // the room's name: a preset's number
constexpr const char* kPublic = "pub";    // "1" listed in the lobby, "0" by code only
constexpr const char* kTables = "tables"; // 1..kMaxTables
// t0..t7: each table's rules (FormatRules).
std::string TableKey(int table);
// Lobby room data: the public room list, pr0..pr15 (PackRoomList).
std::string ListKey(int index);
// Member data.
constexpr const char* kProof = "kp";
constexpr const char* kRequest = "rq";
constexpr const char* kAck = "ak";

constexpr int kLobbyCap = 250;  // members of a lobby channel
constexpr int kRoomCap = 16;    // members of a custom room
constexpr int kMaxTables = 8;
constexpr int kDefaultTables = 4;
constexpr int kListKeys = 16;
constexpr size_t kMaxValue = 512; // Weyvelength's room data value limit

// --- proof ---
// The signed text: "sfvr-keeper 1 <room code> <member id>".
std::string ProofMessage(std::string_view room, uint32_t member);
std::string Base64Encode(const uint8_t* data, size_t size);
bool Base64Decode(std::string_view text, std::vector<uint8_t>* out);

// --- room names ---
// Twelve presets (user, 2026-11-02: no free text); sent as their number so a
// client can show them in its own language later.
constexpr int kPresetCount = 12;
const char* PresetName(int preset); // "Casual sets"; "Room" when out of range

// --- requests (member data `rq`) ---
// "<seq>:<kind>[:<arg>]..."; args hold no ':'.
//   room:<tables>:<public 0|1>:<preset>   create a custom room; answered with
//                                         its code (the requester is its owner)
struct Request {
    uint32_t seq = 0;
    std::string kind;
    std::vector<std::string> args;
};
std::string FormatRequest(const Request& request);
bool ParseRequest(std::string_view text, Request* out);

struct RoomSettings {
    int tables = kDefaultTables;
    bool isPublic = true;
    int preset = 0;
};
Request RoomRequest(uint32_t seq, const RoomSettings& settings);
bool ParseRoomSettings(const Request& request, RoomSettings* out);

// --- answers (P2P datagrams from the keeper) ---
// "SFVK1\n<seq>\n<kind>\n<ok|no>\n<text>": `text` is the room code, or what
// went wrong. The magic keeps them apart from session packets.
struct Answer {
    uint32_t seq = 0;
    std::string kind;
    bool ok = false;
    std::string text;
};
std::string FormatAnswer(const Answer& answer);
bool ParseAnswer(const uint8_t* data, size_t size, Answer* out);

// --- the public room list (lobby room data pr0..pr15) ---
struct ListedRoom {
    std::string code;
    int preset = 0;
    std::string owner;       // the owner's name
    int members = 0;         // the keeper not counted
    int tables = 1;
    int openSeats = 0;       // seats nobody sits in, over all tables
    std::string rules;       // the first table's (FormatRules)
    std::vector<std::string> faces; // up to six character codes of members
    bool operator==(const ListedRoom&) const = default;
};
// Rooms with open seats first, then full tables with room to queue, then
// full rooms; the most members first within each.
void SortRoomList(std::vector<ListedRoom>* rooms);
// The values for pr0..pr15: whole entries only, as many as fit in each
// value; rooms past the 16th value are left out (reachable by code). Fewer
// than kListKeys values when the list is short (clear the rest).
std::vector<std::string> PackRoomList(const std::vector<ListedRoom>& rooms);
// From a lobby's room data; entries that do not parse are skipped.
std::vector<ListedRoom> UnpackRoomList(const std::map<std::string, std::string>& data);

// --- chat ---
// Room chat limits (user, 2026-11-02): 200 characters, one message a second
// with a burst of 3. The keeper kicks a member who keeps going past them; the
// mod checks before it sends.
constexpr size_t kChatMaxChars = 200;
size_t Utf8Length(std::string_view text); // in characters (code points)
class ChatLimiter {
public:
    // True if a message sent at `nowMs` is within the limits (and counts it).
    bool Allow(int64_t nowMs, std::string_view text);
    // Messages refused in the last kStrikeWindowMs: the keeper kicks at
    // kKickStrikes.
    int Strikes(int64_t nowMs) const;
    static constexpr int kBurst = 3;
    static constexpr int64_t kRefillMs = 1000;
    static constexpr int64_t kStrikeWindowMs = 10000;
    static constexpr int kKickStrikes = 5;

private:
    double tokens_ = kBurst;
    int64_t lastMs_ = -1;
    std::vector<int64_t> strikes_;
};

// --- a public room's detail (request peek:<code>) ---
// What the custom rooms screen's panel shows for the focused public room:
// each table's rules and the players. Answered in parts (a datagram holds
// 1024 bytes; 16 names of up to 96 bytes do not fit one): each part's text is
// "<i>/<n>\x1D<records>", records split by \x1E: "t<rules>" per table, then
// "p<chara>\x1F<name>" per player. The parts share the request's sequence
// number; the player acknowledges once it has them all.
struct RoomDetail {
    std::string code;
    std::vector<std::string> tables; // rules (FormatRules)
    struct Person {
        std::string name;
        std::string chara;
        bool operator==(const Person&) const = default;
    };
    std::vector<Person> people;
};
Request PeekRequest(uint32_t seq, std::string_view code);
std::vector<std::string> PackRoomDetail(const RoomDetail& detail, size_t partBytes = 800);
// One part: false if it is not one.
bool ReadDetailPart(std::string_view text, int* part, int* parts, std::string* records);
// The parts, in order (all of them): the detail.
bool JoinRoomDetail(const std::vector<std::string>& records, RoomDetail* out);

// --- the name filter (public rooms) ---
// A name the word list rejects shows as "Player N" in public rooms (user,
// 2026-11-02). Compared folded: lower case, digits and symbols read as the
// letters they stand for (0 o, 1 i, 3 e, 4 a, 5 s, 7 t, @ a, $ s), and
// anything else that is not a letter dropped, so "B.a d" and "b4d" are "bad".
std::string FoldName(std::string_view name);
// `words` folded already (FoldName); empty words are ignored.
bool NameRejected(std::string_view name, const std::vector<std::string>& words);
// Room data `mask`: the members whose names show as "Player N", with their
// N (the keeper's count, so every client shows the same): "12=1,34=3".
constexpr const char* kMask = "mask";
std::string FormatMask(const std::map<uint32_t, int>& numbers);
std::map<uint32_t, int> ParseMask(std::string_view text);

// --- ping between room members (P2P datagrams) ---
// "SFVP" + 1 (ping) or 2 (pong) + the sender's 8-byte clock, echoed back.
constexpr size_t kPingSize = 13;
void FormatPing(bool pong, uint64_t clockUs, uint8_t (&out)[kPingSize]);
bool ParsePing(const uint8_t* data, size_t size, bool* pong, uint64_t* clockUs);

// --- tables (M4) ---
// Each table's state (room data s0..s7) and queue (q0..q7, member ids in
// order), written by the keeper. A table is Open (seats fill from the
// queue, or by a player sitting down), Call (both seats taken: each player
// has the your-turn card and readies), Connecting (both ready: P1 hosts the
// match room, the keeper hands out the tickets), Playing (the set runs in the
// match room), After (the set ended: the after-set panels, then rotation).
enum class TablePhase : char { Open = 'o', Call = 'c', Connecting = 'n', Playing = 'p', After = 'a' };
struct TableState {
    TablePhase phase = TablePhase::Open;
    uint32_t seat[2] = {0, 0}; // P1, P2 (0 open)
    bool ready[2] = {false, false};
    uint8_t wins[2] = {0, 0};  // the running set's score
    std::string code;          // the match room (Connecting, Playing)
    uint32_t stayer = 0;       // who stayed on from the last set (keeps their seat)
    int streak = 0;            // sets in a row the stayer has played here
    // The last set (After, and the room's stripe): its winner and loser, the score.
    uint32_t lastWinner = 0, lastLoser = 0;
    uint8_t lastScore[2] = {0, 0}; // winner's, loser's
    // Each call is numbered (a call again for the same two is a new one) and
    // says how long it lasts: longer after a set that broke off, while its
    // players' games are still coming back from the battle.
    uint32_t call = 0;
    uint8_t callSeconds = 0;
    bool operator==(const TableState&) const = default;
};
std::string StateKey(int table); // "s0"
std::string QueueKey(int table); // "q0"
std::string FormatTable(const TableState& s);
bool ParseTable(std::string_view text, TableState* out);
std::string FormatIds(const std::vector<uint32_t>& ids);
std::vector<uint32_t> ParseIds(std::string_view text);

// Seats from the queue: each open seat (P1 first) takes the queue's head;
// both taken makes it Call (nobody ready yet), else it stays Open.
void FillSeats(TableState* s, std::vector<uint32_t>* queue);
// A set ended, `winnerSeat` (0 P1, 1 P2) with `games` (P1's, P2's): who stays
// (keeping their seat) and who goes to the back of the queue, by the table's
// rotation (rules.h): winner stays or loser stays, each at most
// rules.staysLimit sets in a row (0: no limit), or round robin (both go).
// Leaves the table After with the seats of those who left open.
void ApplySetResult(const Rules& rules, int winnerSeat, const uint8_t (&games)[2], TableState* s, std::vector<uint32_t>* queue);
// Takes a member off the table: their seat, their place in the queue. True
// if they were on it.
bool LeaveTable(uint32_t id, TableState* s, std::vector<uint32_t>* queue);

// Table requests (member data `rq` in a custom room):
//   seat:<table>:<0|1|-1>   sit at an open seat (-1: either)
//   stand                   get up (Open), or out of the call
//   queue:<table>           into its queue (straight into a seat when one is open)
//   unqueue                 out of any queue
//   ready:<table>           the your-turn card's Ready
//   decline:<table>         the your-turn card's Leave queue
//   code:<table>:<code>     P1's: the match room is open on this code
//   score:<table>:<w1>:<w2> P1's: the set's score after each game
//   result:<table>:<w1>:<w2>  P1's (or P2's when P1 is gone): the set ended; w1 = w2 = 0: it failed
//   rules:<table>:<rules>   the owner's: a table's rules, from its next set
//   kick:<member>           the owner's: out of the room (M6)
// The keeper's ticket (answer kind "ticket"), privately to each player:
//   host|<table>|<token>|<rules>          P1: open the match room
//   join|<table>|<code>|<token>|<rules>   P2: join it
struct TableTicket {
    bool host = false;
    int table = 0;
    std::string code;   // join
    uint64_t token = 0; // the P2 seat's token (lobby.h Hello)
    std::string rules;  // FormatRules
};
std::string FormatTicket(const TableTicket& t);
bool ParseTicket(std::string_view text, TableTicket* out);

// --- ownership ---
// The member who gets a room when its owner leaves: the longest present
// (`joinOrder`, oldest first), not `leaving`, and not the keeper; 0 if none.
uint32_t NextOwner(const std::vector<uint32_t>& joinOrder, uint32_t leaving, uint32_t keeper);

} // namespace sfv::keeper
