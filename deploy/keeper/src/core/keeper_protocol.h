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
// Listing slots and room data of a presence room (the desk, a custom room).
constexpr const char* kKind = "kind";     // "lobby" (the desk) or "custom"
constexpr const char* kKindLobby = "lobby";
constexpr const char* kKindCustom = "custom";
constexpr const char* kBuild = "build";   // the lobby protocol (lobby::kProtocol)
constexpr const char* kCount = "count";   // members, the keeper not counted (desk listing)
// Custom room data.
constexpr const char* kOwner = "owner";   // the owner's member id
constexpr const char* kTitle = "title";  // the room's title (FormatTitle)
constexpr const char* kPublic = "pub";    // "1" listed in the lobby, "0" by code only
constexpr const char* kTables = "tables"; // 1..kMaxTables
// t0..t7: each table's rules (FormatRules).
std::string TableKey(int table);
// Member data.
constexpr const char* kProof = "kp";
constexpr const char* kRequest = "rq";
constexpr const char* kAck = "ak";

constexpr int kLobbyCap = 250;  // members of a desk (the lobby channel)
constexpr int kRoomCap = 16;    // members of a custom room
constexpr int kMaxTables = 8;
constexpr int kDefaultTables = 4;

// --- proof ---
// The signed text: "sfvr-keeper 1 <room code> <member id>".
std::string ProofMessage(std::string_view room, uint32_t member);
std::string Base64Encode(const uint8_t* data, size_t size);
bool Base64Decode(std::string_view text, std::vector<uint8_t>* out);

// --- characters ---
// The roster as the select screen has it (Eleven left out), and each one's
// name in English (the rooms API; the game's own are all capitals).
constexpr int kCharacterCount = 45;
const char* CharacterCode(int index);              // "RYU"; "" out of range
int CharacterIndex(std::string_view code);         // -1 when not on the roster
const char* CharacterName(std::string_view code);  // "Chun-Li"; "" when not on the roster

// --- room titles ---
// SFV's Battle Lounge comments (the game's FixedPhraseCommentSpec table, by
// its Id; user, 2026-11-06): 0 (no title), 3 ("(Character 1) user looking for
// (Character 2)!") and 5-18. No free text (user, 2026-11-02): a game sends the
// Id and, for 3, two character codes; the keeper checks them and writes the
// English text (the rooms API), and each game shows the game's own text, in
// its language.
struct RoomTitle {
    int id = 0;
    std::string chara[2]; // Id 3's characters (CharacterCode); empty otherwise
    bool operator==(const RoomTitle&) const = default;
};
constexpr int kTitleCount = 16;
int TitleId(int index);                 // the index-th offered title's Id (form order); 0 out of range
int TitleIndex(int id);                 // -1 when the Id is not offered
int TitleCharacters(int id);            // how many characters it names (2 for 3, else 0)
bool ValidTitle(const RoomTitle& title); // an offered Id with exactly its characters, each on the roster
std::string FormatTitle(const RoomTitle& title);              // "5", "3,RYU,KEN"
bool ParseTitle(std::string_view text, RoomTitle* out);       // valid titles only
const char* TitleTemplate(int id);      // the English text with {common1}/{common2}; "" for 0 and unknown Ids
std::string TitleText(const RoomTitle& title); // in English with the names; "" for no title

// --- requests (member data `rq`) ---
// "<seq>:<kind>[:<arg>]..."; args hold no ':'.
//   room:<tables>:<public 0|1>:<title>    create a custom room (FormatTitle);
//                                         answered with its code (the requester
//                                         is its owner)
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
    RoomTitle title;
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

// A country code as the rooms server gives it: two letters A-Z, else "".
std::string CleanCountry(std::string_view code);

// --- the public room list (lobby room data pr0..pr15) ---
struct ListedRoom {
    std::string code;
    RoomTitle title;
    std::string owner;       // the owner's name
    std::string ownerCountry; // the owner's ISO 3166 alpha-2 (member data `@cc`); "" unknown or hidden
    int members = 0;         // the keeper not counted
    int tables = 1;
    int openSeats = 0;       // seats nobody sits in, over all tables
    std::string rules;       // the first table's (FormatRules)
    std::vector<std::string> faces; // up to six character codes of members
    // Every member, in join order, for the rooms API only (FormatRoomListJson;
    // the lobby's keys have no room for them).
    struct Player {
        std::string name;    // as the room shows it (the name filter's "Player N")
        std::string country; // ISO 3166 alpha-2, the rooms server's lookup (member data `@cc`); "" unknown
        std::string face;    // the character code (member data `ch`); "" none
        bool operator==(const Player&) const = default;
    };
    std::vector<Player> players;
    bool operator==(const ListedRoom&) const = default;
};
// Rooms with open seats first, then full tables with room to queue, then
// full rooms; the most members first within each.
void SortRoomList(std::vector<ListedRoom>* rooms);

// --- the public list in the rooms server's room listing ---
// (docs/room-listing-plan.md) Public custom rooms are listed by the rooms
// server itself: anyone connected lists them (ListRooms) without joining a
// room. The keeper, as each room's host, sets four listing slots:
constexpr const char* kListingKind = "k";  // "c<protocol>": what players filter on
constexpr const char* kListingRoom = "a";  // "<title>|<members>|<tables>|<open seats>|<rules>|<faces>"
constexpr const char* kListingOwner = "o"; // "<country>|<owner's name>"
constexpr const char* kListingSig = "s";   // the keeper's signature (base64) of ListingMessage
// The desk (the lobby channel, joined only to ask the keeper for something)
// is listed with kKind "lobby", kBuild, kCount and these two:
constexpr const char* kDeskSummary = "r";  // FormatDeskSummary
// (kListingSig: the signature of DeskMessage). Players poll the desk's
// listing and list the rooms again only when its revision moves.
constexpr size_t kListingValueMax = 128;   // Weyvelength's listing value limit
using Listing = std::map<std::string, std::string>;
std::string ListingKindValue(uint16_t protocol); // "c16"
// A public room's slots, its signature left out: k, a and o (o's name cut
// to fit 128 bytes).
Listing RoomListing(const ListedRoom& room, uint16_t protocol);
// What the keeper signs for a room: "sfvr-list 1 <code>\n<k>\n<a>\n<o>".
std::string ListingMessage(std::string_view code, const Listing& listing);
// A listed room's slots (players check kListingSig apart): false if it is
// not a custom room of this protocol or they do not parse.
bool ParseRoomListing(std::string_view code, const Listing& listing, uint16_t protocol, ListedRoom* out);
// The desk's summary: its revision (up whenever a public room's listing
// changes), the public rooms and the players in them.
struct DeskSummary {
    uint64_t rev = 0;
    int publicRooms = 0;
    int players = 0;
    bool operator==(const DeskSummary&) const = default;
};
std::string FormatDeskSummary(const DeskSummary& summary); // "<rev>|<rooms>|<players>"
bool ParseDeskSummary(std::string_view text, DeskSummary* out);
// What the keeper signs for a desk: "sfvr-desk 1 <code>\n<build>\n<count>\n<r>".
std::string DeskMessage(std::string_view code, const Listing& listing);

// The rooms API's document (the keeper's --status file, which sfvr-server's
// rooms publisher sends on): the public list as the lobby shows it, and three
// public totals (user, 2026-11-06): players online, how many of them are in
// public rooms, and the public rooms. Nothing else about private rooms.
struct RoomTotals {
    int online = 0;        // every hub on the server: the lobby channel's members and every custom room's
    int inPublicRooms = 0; // members of the listed rooms
    int publicRooms = 0;   // listed
};
// {"version":1,"updated":<unix ms>,"totals":{"online":..,"in_public_rooms":..,
// "public_rooms":..},"rooms":[{"code":..,
// "title":"Beginners Welcome!"|null,"title_id":10,"title_characters":[..],"owner":..,
// "members":..,"capacity":16,"tables":..,
// "open_seats":..,"rules":..,"faces":[..],"players":[{"name":..,"country":"DE"|null,
// "face":"RYU"|null}]}]}; strings escaped for JSON.
std::string FormatRoomListJson(const std::vector<ListedRoom>& rooms, const RoomTotals& totals, int64_t unixMs);

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
// "p<chara>\x1F<country>\x1F<name>" per player (country: ISO 3166 alpha-2 or
// empty). The parts share the request's sequence
// number; the player acknowledges once it has them all.
struct RoomDetail {
    std::string code;
    std::vector<std::string> tables; // rules (FormatRules)
    struct Person {
        std::string name;
        std::string chara;
        std::string country; // ISO 3166 alpha-2; "" unknown or hidden
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

// --- the word filter (public rooms: names and chat) ---
// (docs/word-filter-plan.md) A name the word list rejects shows as "Player
// 1234" in public rooms; a chat line has the words it rejects starred. Both
// for the viewer, who may turn it off; the rooms API always masks names.
// Compared folded: lower case, digits and symbols read as the letters they
// stand for (0 o, 1 i, 3 e, 4 a, 5 s, 7 t, @ a, $ s), and anything else that
// is not a letter dropped, so "B.a d" and "b4d" are "bad".
std::string FoldName(std::string_view name);
// The list's entries: folded, a phrase's words joined by '_', and a trailing
// '*' for "also any word starting with it" (chat). From a file: one entry a
// line, blank lines and '#' comments skipped, entries that fold to nothing
// dropped.
std::vector<std::string> ParseWordFile(std::string_view text);
// One entry as the file would have it ("Bad*" -> "bad*"); empty if it folds
// to nothing.
std::string WordEntry(std::string_view line);
// A name: an entry of 5 letters or more anywhere in it (names run words
// together: "xXbadwordXx"); a shorter one only as a whole word of the name
// (split at what folding drops), so short entries do not catch ordinary
// names that contain them.
bool NameRejected(std::string_view name, const std::vector<std::string>& words);
// A chat line with each rejected word's characters replaced by '*' (whole
// UTF-8 characters, so it keeps its length on screen): a word, or a run of
// up to six words in a row matching a phrase entry, whose folded form is an entry, or a word that
// starts with a '*' entry. Words split at spaces and punctuation, so only
// whole words match ("class" passes an entry that is part of it).
std::string CensorChat(std::string_view text, const std::vector<std::string>& words);
// The list in a public room's data, for the players' games: kWordListHash
// (WordListHash) and the entries over wl0, wl1... (PackWordList: joined by
// ',', 512 bytes a value at most, kWordListKeys values at most).
constexpr const char* kWordListHash = "wlv";
std::string WordListKey(int index); // "wl0"
constexpr int kWordListKeys = 24;
std::vector<std::string> PackWordList(const std::vector<std::string>& words);
std::vector<std::string> UnpackWordList(const std::vector<std::string>& values);
std::string WordListHash(const std::vector<std::string>& words);
// Room data `mask`: the members whose names show as "Player <number>", with
// their number (1000-9999, the keeper's pick, the same for every viewer):
// "12=4821,34=1307".
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
    // The keeper's relay room for the running set (M5): viewers watch there,
    // never in the match room. It stays while the relay serves viewers behind.
    std::string watch;
    // Playing with nobody else waiting for the table (its queue empty, both
    // players in the room): when the set is won its players may play on
    // into the next set without leaving the match (request again).
    bool again = false;
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
//   again:<table>:<w1>:<w2>  P1's: the set is won (w1, w2) and both chose to play on: with
//                           nobody waiting (TableState::again) the keeper records it and the
//                           same two start the next set in the same match room (answer
//                           kind "again" to both: ok with the next set's rules; else no, and
//                           the set's result stands as a result's would)
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
