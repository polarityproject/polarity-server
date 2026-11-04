#include "core/rules.h"

#include <charconv>
#include <vector>

namespace sfv {

namespace {

bool Number(std::string_view text, int* out)
{
    if (text.empty() || text.size() > 3) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
    return ec == std::errc() && end == text.data() + text.size();
}

bool StaysLimitAllowed(int n) { return n == 0 || n == 3 || n == 5; }

} // namespace

std::string FormatRules(const Rules& r)
{
    std::string out = "r" + std::to_string(kRulesVersion) + " ft" + std::to_string(r.firstTo) + " bo" + std::to_string(r.bestOf) +
                      " t" + std::to_string(r.timer) + " ";
    switch (r.rotation) {
    case Rotation::WinnerStays: out += "ws" + std::to_string(r.staysLimit); break;
    case Rotation::LoserStays: out += "ls" + std::to_string(r.staysLimit); break;
    case Rotation::RoundRobin: out += "rr"; break;
    }
    return out;
}

bool ParseRules(std::string_view text, Rules* out, std::string* error)
{
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::vector<std::string_view> words;
    while (!text.empty()) {
        const size_t space = text.find(' ');
        const std::string_view word = text.substr(0, space);
        if (!word.empty()) words.push_back(word);
        if (space == std::string_view::npos) break;
        text.remove_prefix(space + 1);
    }
    if (words.empty() || words[0].size() < 2 || words[0][0] != 'r') return fail("not rules");
    int version = 0;
    if (!Number(words[0].substr(1), &version)) return fail("not rules");
    if (version != kRulesVersion) return fail("rules version " + std::to_string(version) + " (this build reads " + std::to_string(kRulesVersion) + ")");
    Rules r;
    bool firstTo = false, bestOf = false, timer = false, rotation = false;
    for (size_t i = 1; i < words.size(); i++) {
        const std::string_view w = words[i];
        int n = 0;
        auto field = [&](std::string_view prefix, bool* seen) {
            if (w.substr(0, prefix.size()) != prefix) return false;
            if (*seen) return false;
            *seen = true;
            return true;
        };
        if (w.substr(0, 2) == "ft") {
            if (!field("ft", &firstTo) || !Number(w.substr(2), &n) || n < 1 || n > 20) return fail("first to: " + std::string(w));
            r.firstTo = static_cast<uint8_t>(n);
        }
        else if (w.substr(0, 2) == "bo") {
            if (!field("bo", &bestOf) || !Number(w.substr(2), &n) || (n != 1 && n != 3 && n != 5)) return fail("rounds: " + std::string(w));
            r.bestOf = static_cast<uint8_t>(n);
        }
        else if (w.substr(0, 1) == "t") {
            if (!field("t", &timer) || !Number(w.substr(1), &n) || (n != 99 && n != 60 && n != 30)) return fail("timer: " + std::string(w));
            r.timer = static_cast<uint8_t>(n);
        }
        else if (w == "rr") {
            if (!field("rr", &rotation)) return fail("rotation twice");
            r.rotation = Rotation::RoundRobin;
            r.staysLimit = 0;
        }
        else if (w.substr(0, 2) == "ws" || w.substr(0, 2) == "ls") {
            if (!field(w.substr(0, 2), &rotation) || !Number(w.substr(2), &n) || !StaysLimitAllowed(n)) return fail("rotation: " + std::string(w));
            r.rotation = w[0] == 'w' ? Rotation::WinnerStays : Rotation::LoserStays;
            r.staysLimit = static_cast<uint8_t>(n);
        }
        else {
            return fail("unknown field " + std::string(w));
        }
    }
    if (!firstTo || !bestOf || !timer || !rotation) return fail("a field is missing");
    *out = r;
    return true;
}

std::string DescribeRules(const Rules& r)
{
    std::string out = "First to " + std::to_string(r.firstTo) + " \xC2\xB7 ";
    switch (r.rotation) {
    case Rotation::WinnerStays: out += "winner stays"; break;
    case Rotation::LoserStays: out += "loser stays"; break;
    case Rotation::RoundRobin: out += "round robin"; break;
    }
    if (r.rotation != Rotation::RoundRobin) out += r.staysLimit ? ", up to " + std::to_string(r.staysLimit) : ", no limit";
    out += " \xC2\xB7 " + std::to_string(r.timer) + " seconds";
    if (r.bestOf != 3) out += r.bestOf == 1 ? " \xC2\xB7 one round" : " \xC2\xB7 best of 5";
    return out;
}

} // namespace sfv
