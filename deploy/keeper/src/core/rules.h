#pragma once
// A table's rules (docs/custom-rooms-plan.md, M2; the Table rules screen),
// as they travel in room data and the match ticket: one short line of text,
// versioned, far under Weyvelength's 512-byte values:
//
//   "r1 ft2 bo3 t99 ws3"
//
//   r1    the format's version
//   ftN   first to N games wins the set (1 to 20)
//   boN   rounds per game: best of 1, 3 or 5 (battle data +0x04 and +0x08)
//   tN    the round timer in seconds: 99, 60 or 30 (+0x0C, in frames; it has
//         no "infinite": the M0 spike)
//   wsN   rotation: winner stays, at most N sets in a row (0: no limit);
//   lsN   loser stays, the same; rr: round robin
//
// No stage rule (the stage is P1's preferred one), no input delay cap and no
// spectator switch (user, 2026-11-02). A later version adds fields; a build
// refuses rules of a version it does not know rather than guess.

#include <cstdint>
#include <string>
#include <string_view>

namespace sfv {

enum class Rotation : uint8_t { WinnerStays, LoserStays, RoundRobin };

struct Rules {
    uint8_t firstTo = 2;    // games to win the set
    uint8_t bestOf = 3;     // rounds per game
    uint8_t timer = 99;     // seconds
    Rotation rotation = Rotation::WinnerStays;
    uint8_t staysLimit = 3; // sets in a row for whoever stays (0: no limit); not used with round robin
    bool operator==(const Rules&) const = default;
};

constexpr int kRulesVersion = 1;

std::string FormatRules(const Rules& rules);
// False (and why) for text that is not rules this build knows: another
// version, a value the game does not allow, a field missing or twice.
bool ParseRules(std::string_view text, Rules* out, std::string* error = nullptr);

// "First to 2 · winner stays, up to 3 · 99 seconds" (the table's summary line).
std::string DescribeRules(const Rules& rules);

} // namespace sfv
