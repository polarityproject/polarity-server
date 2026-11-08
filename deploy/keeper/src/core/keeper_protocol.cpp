#include "core/keeper_protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace sfv::keeper {

namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kAnswerMagic = "SFVK1";

bool Number(std::string_view text, int64_t* out)
{
    if (text.empty() || text.size() > 10) return false;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), *out);
    return ec == std::errc() && end == text.data() + text.size();
}

bool Int(std::string_view text, int* out)
{
    int64_t n = 0;
    if (!Number(text, &n) || n < INT32_MIN || n > INT32_MAX) return false;
    *out = static_cast<int>(n);
    return true;
}

std::vector<std::string_view> Split(std::string_view text, char sep)
{
    std::vector<std::string_view> parts;
    size_t from = 0;
    while (true) {
        const size_t at = text.find(sep, from);
        parts.push_back(text.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from));
        if (at == std::string_view::npos) break;
        from = at + 1;
    }
    return parts;
}

// Names and codes in the list: no tabs, newlines or commas (the list's
// separators), nothing below space.
std::string Clean(std::string_view text)
{
    std::string s;
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || ch == ',') continue;
        s += ch;
    }
    return s;
}

std::string Slot(const Listing& listing, const char* key)
{
    const auto it = listing.find(key);
    return it != listing.end() ? it->second : std::string();
}

// Cut to `max` bytes at a whole UTF-8 character.
std::string CutUtf8(std::string s, size_t max)
{
    if (s.size() <= max) return s;
    s.resize(max);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) s.pop_back();
    return s;
}

} // namespace

std::string CleanCountry(std::string_view code)
{
    if (code.size() != 2) return {};
    for (char c : code) {
        if (c < 'A' || c > 'Z') return {};
    }
    return std::string(code);
}

std::string TableKey(int table) { return "t" + std::to_string(table); }

std::string ProofMessage(std::string_view room, uint32_t member)
{
    return "sfvr-keeper 1 " + std::string(room) + " " + std::to_string(member);
}

std::string Base64Encode(const uint8_t* data, size_t size)
{
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                           (i + 2 < size ? uint32_t(data[i + 2]) : 0);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < size ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kAlphabet[n & 63] : '=';
    }
    return out;
}

bool Base64Decode(std::string_view text, std::vector<uint8_t>* out)
{
    out->clear();
    uint32_t n = 0;
    int bits = 0;
    size_t pad = 0;
    for (char ch : text) {
        if (ch == '=') {
            pad++;
            continue;
        }
        if (pad) return false; // data after padding
        const char* at = std::char_traits<char>::find(kAlphabet, 64, ch);
        if (!at) return false;
        n = (n << 6) | uint32_t(at - kAlphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((n >> bits) & 0xFF));
        }
    }
    return pad <= 2 && (text.size() % 4 == 0);
}

namespace {

// In the game's own order; names as the game spells them, in normal case.
const struct {
    const char* code;
    const char* name;
} kCharacters[kCharacterCount] = {
    {"RYU", "Ryu"},     {"VEG", "M. Bison"}, {"CNL", "Chun-Li"}, {"KEN", "Ken"},      {"KRN", "Karin"},   {"ZGF", "Zangief"},
    {"DSM", "Dhalsim"}, {"NSH", "Nash"},     {"BLR", "Vega"},    {"JRI", "Juri"},     {"BRD", "Birdie"},  {"RMK", "R. Mika"},
    {"RSD", "Rashid"},  {"FAN", "F.A.N.G"},  {"LAR", "Laura"},   {"NCL", "Necalli"},  {"CMY", "Cammy"},   {"GUL", "Guile"},
    {"IBK", "Ibuki"},   {"BSN", "Balrog"},   {"URN", "Urien"},   {"ALX", "Alex"},     {"Z21", "Akuma"},   {"Z20", "Kolin"},
    {"Z22", "Ed"},      {"Z23", "Menat"},    {"Z24", "Abigail"}, {"Z25", "Zeku"},     {"Z26", "Sakura"},  {"Z27", "Blanka"},
    {"Z28", "Falke"},   {"Z29", "Cody"},     {"Z30", "G"},       {"Z31", "Sagat"},    {"Z32", "Kage"},    {"Z33", "Poison"},
    {"Z34", "E. Honda"}, {"Z35", "Lucia"},   {"Z36", "Gill"},    {"Z37", "Seth"},     {"Z38", "Dan"},     {"Z39", "Rose"},
    {"Z40", "Oro"},     {"Z41", "Akira"},    {"Z42", "Luke"}};

// The offered titles, in the form's order: the game's FixedPhraseCommentSpec
// Id and its English CommentText (trailing spaces trimmed).
const struct {
    int id;
    const char* text;
} kTitles[kTitleCount] = {
    {0, ""},
    {3, "{common1} user looking for {common2}!"},
    {5, "Hardcore match! Main character only!"},
    {6, "Sub-character practice is allowed"},
    {7, "Serious match!"},
    {8, "Seeking someone stronger than me!"},
    {9, "If it's to win, anything goes."},
    {10, "Beginners Welcome!"},
    {11, "Anyone will do."},
    {12, "Let's have some casual fun!"},
    {13, "Let's hunker down!"},
    {14, "Fighting all night! Let's do this!"},
    {15, "I don't care if I win or lose!"},
    {16, "Random character matches"},
    {17, "I'm streaming."},
    {18, "I'm recording commentary"}};

} // namespace

const char* CharacterCode(int index) { return index >= 0 && index < kCharacterCount ? kCharacters[index].code : ""; }

int CharacterIndex(std::string_view code)
{
    for (int i = 0; i < kCharacterCount; i++) {
        if (code == kCharacters[i].code) return i;
    }
    return -1;
}

const char* CharacterName(std::string_view code)
{
    const int i = CharacterIndex(code);
    return i >= 0 ? kCharacters[i].name : "";
}

int TitleId(int index) { return index >= 0 && index < kTitleCount ? kTitles[index].id : 0; }

int TitleIndex(int id)
{
    for (int i = 0; i < kTitleCount; i++) {
        if (kTitles[i].id == id) return i;
    }
    return -1;
}

int TitleCharacters(int id) { return id == 3 ? 2 : 0; }

bool ValidTitle(const RoomTitle& title)
{
    if (TitleIndex(title.id) < 0) return false;
    const int n = TitleCharacters(title.id);
    for (int i = 0; i < 2; i++) {
        if (i < n ? CharacterIndex(title.chara[i]) < 0 : !title.chara[i].empty()) return false;
    }
    return true;
}

std::string FormatTitle(const RoomTitle& title)
{
    std::string s = std::to_string(title.id);
    for (int i = 0; i < TitleCharacters(title.id); i++) s += "," + title.chara[i];
    return s;
}

bool ParseTitle(std::string_view text, RoomTitle* out)
{
    const auto f = Split(text, ',');
    RoomTitle t;
    if (f.empty() || !Int(f[0], &t.id)) return false;
    if (static_cast<int>(f.size()) != 1 + TitleCharacters(t.id)) return false;
    for (size_t i = 1; i < f.size(); i++) t.chara[i - 1] = std::string(f[i]);
    if (!ValidTitle(t)) return false;
    *out = std::move(t);
    return true;
}

const char* TitleTemplate(int id)
{
    const int i = TitleIndex(id);
    return i >= 0 ? kTitles[i].text : "";
}

std::string TitleText(const RoomTitle& title)
{
    std::string s = TitleTemplate(title.id);
    for (int i = 0; i < 2; i++) {
        const std::string key = "{common" + std::to_string(i + 1) + "}";
        const size_t at = s.find(key);
        if (at != std::string::npos) s.replace(at, key.size(), CharacterName(title.chara[i]));
    }
    return s;
}

std::string FormatRequest(const Request& request)
{
    std::string s = std::to_string(request.seq) + ":" + request.kind;
    for (const std::string& a : request.args) s += ":" + a;
    return s;
}

bool ParseRequest(std::string_view text, Request* out)
{
    const auto parts = Split(text, ':');
    if (parts.size() < 2 || parts[1].empty()) return false;
    int64_t seq = 0;
    if (!Number(parts[0], &seq) || seq <= 0 || seq > UINT32_MAX) return false;
    Request r;
    r.seq = static_cast<uint32_t>(seq);
    r.kind = std::string(parts[1]);
    for (size_t i = 2; i < parts.size(); i++) r.args.emplace_back(parts[i]);
    *out = std::move(r);
    return true;
}

Request RoomRequest(uint32_t seq, const RoomSettings& settings)
{
    return {seq, "room", {std::to_string(settings.tables), settings.isPublic ? "1" : "0", FormatTitle(settings.title)}};
}

bool ParseRoomSettings(const Request& request, RoomSettings* out)
{
    if (request.kind != "room" || request.args.size() != 3) return false;
    RoomSettings s;
    int pub = 0;
    if (!Int(request.args[0], &s.tables) || !Int(request.args[1], &pub) || !ParseTitle(request.args[2], &s.title)) return false;
    if (s.tables < 1 || s.tables > kMaxTables || (pub != 0 && pub != 1)) return false;
    s.isPublic = pub == 1;
    *out = s;
    return true;
}

std::string FormatAnswer(const Answer& answer)
{
    std::string text;
    for (char ch : answer.text) text += ch == '\n' ? ' ' : ch;
    return std::string(kAnswerMagic) + "\n" + std::to_string(answer.seq) + "\n" + answer.kind + "\n" +
           (answer.ok ? "ok" : "no") + "\n" + text;
}

bool ParseAnswer(const uint8_t* data, size_t size, Answer* out)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (text.substr(0, kAnswerMagic.size()) != kAnswerMagic) return false;
    const auto f = Split(text, '\n');
    if (f.size() != 5 || f[0] != kAnswerMagic || (f[3] != "ok" && f[3] != "no")) return false;
    int64_t seq = 0;
    if (!Number(f[1], &seq) || seq <= 0 || seq > UINT32_MAX) return false;
    out->seq = static_cast<uint32_t>(seq);
    out->kind = std::string(f[2]);
    out->ok = f[3] == "ok";
    out->text = std::string(f[4]);
    return true;
}

void SortRoomList(std::vector<ListedRoom>* rooms)
{
    // 0: open seats, 1: room to queue, 2: full.
    auto group = [](const ListedRoom& r) { return r.members >= kRoomCap ? 2 : r.openSeats > 0 ? 0 : 1; };
    std::stable_sort(rooms->begin(), rooms->end(), [&](const ListedRoom& a, const ListedRoom& b) {
        if (group(a) != group(b)) return group(a) < group(b);
        return a.members > b.members;
    });
}

std::string ListingKindValue(uint16_t protocol, std::string_view version)
{
    std::string kind = "c" + std::to_string(protocol);
    if (!version.empty()) kind += "/" + Clean(std::string(version.substr(0, kVersionMax)));
    return kind;
}

Listing RoomListing(const ListedRoom& r, uint16_t protocol)
{
    std::string faces;
    for (size_t i = 0; i < r.faces.size() && i < 6; i++) {
        if (i) faces += ',';
        faces += Clean(r.faces[i]);
    }
    std::string rules = Clean(r.rules);
    std::replace(rules.begin(), rules.end(), '|', ' ');
    Listing l;
    l[kListingKind] = ListingKindValue(protocol, r.version);
    l[kListingRoom] = CutUtf8(FormatTitle(r.title) + '|' + std::to_string(r.members) + '|' + std::to_string(r.tables) + '|' +
                                  std::to_string(r.openSeats) + '|' + rules + '|' + faces,
                              kListingValueMax);
    l[kListingOwner] = CutUtf8(CleanCountry(r.ownerCountry) + '|' + Clean(r.owner), kListingValueMax);
    return l;
}

std::string ListingMessage(std::string_view code, const Listing& l)
{
    return "sfvr-list 1 " + std::string(code) + "\n" + Slot(l, kListingKind) + "\n" + Slot(l, kListingRoom) + "\n" +
           Slot(l, kListingOwner);
}

bool ParseRoomListing(std::string_view code, const Listing& l, uint16_t protocol, ListedRoom* out)
{
    const std::string kind = Slot(l, kListingKind), plain = ListingKindValue(protocol);
    if (code.empty() || (kind != plain && kind.rfind(plain + "/", 0) != 0)) return false;
    const std::string a = Slot(l, kListingRoom), o = Slot(l, kListingOwner);
    const auto f = Split(a, '|');
    const size_t bar = o.find('|');
    if (f.size() != 6 || bar == std::string::npos) return false;
    ListedRoom r;
    r.code = std::string(code);
    if (!ParseTitle(f[0], &r.title) || !Int(f[1], &r.members) || !Int(f[2], &r.tables) || !Int(f[3], &r.openSeats)) return false;
    r.rules = std::string(f[4]);
    if (!f[5].empty()) {
        for (std::string_view face : Split(f[5], ',')) r.faces.emplace_back(face);
    }
    r.version = kind.size() > plain.size() ? kind.substr(plain.size() + 1) : std::string();
    r.ownerCountry = CleanCountry(std::string_view(o).substr(0, bar));
    r.owner = o.substr(bar + 1);
    *out = std::move(r);
    return true;
}

std::string FormatDeskSummary(const DeskSummary& s)
{
    std::string out = std::to_string(s.rev) + "|" + std::to_string(s.publicRooms) + "|" + std::to_string(s.players);
    std::string versions;
    for (const DeskSummary::Version& v : s.versions) {
        std::string name = Clean(v.version);
        std::erase_if(name, [](char c) { return c == ',' || c == ':' || c == '|'; });
        if (name.empty()) continue;
        const std::string entry = name + ":" + std::to_string(v.publicRooms) + ":" + std::to_string(v.players);
        if (out.size() + 1 + versions.size() + 1 + entry.size() > kListingValueMax) break;
        versions += (versions.empty() ? "" : ",") + entry;
    }
    if (!versions.empty()) out += "|" + versions;
    return out;
}

DeskSummary DeskSummary::For(std::string_view version) const
{
    if (versions.empty()) return *this;
    DeskSummary mine;
    mine.rev = rev;
    for (const Version& v : versions) {
        if (v.version == version) {
            mine.publicRooms = v.publicRooms;
            mine.players = v.players;
        }
    }
    return mine;
}

bool ParseDeskSummary(std::string_view text, DeskSummary* out)
{
    const auto f = Split(text, '|');
    if ((f.size() != 3 && f.size() != 4) || f[0].empty()) return false;
    DeskSummary s;
    for (char c : f[0]) {
        if (c < '0' || c > '9') return false;
        s.rev = s.rev * 10 + static_cast<uint64_t>(c - '0');
    }
    if (!Int(f[1], &s.publicRooms) || !Int(f[2], &s.players)) return false;
    if (f.size() == 4 && !f[3].empty()) {
        for (std::string_view entry : Split(f[3], ',')) {
            const auto p = Split(entry, ':');
            DeskSummary::Version v;
            if (p.size() != 3 || p[0].empty() || !Int(p[1], &v.publicRooms) || !Int(p[2], &v.players)) return false;
            v.version = std::string(p[0]);
            s.versions.push_back(std::move(v));
        }
    }
    *out = s;
    return true;
}

std::string DeskMessage(std::string_view code, const Listing& l)
{
    return "sfvr-desk 1 " + std::string(code) + "\n" + Slot(l, kBuild) + "\n" + Slot(l, kCount) + "\n" + Slot(l, kDeskSummary);
}

namespace {

// A JSON string: quotes, backslashes and control characters escaped, and
// bytes that are not valid UTF-8 (names are players' own) as U+FFFD.
void JsonString(std::string& out, std::string_view s)
{
    out += '"';
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
            else if (c < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                out += esc;
            }
            else out += static_cast<char>(c);
            i++;
            continue;
        }
        const size_t len = c > 0xF4 ? 0 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
        bool ok = len && i + len <= s.size();
        for (size_t k = 1; ok && k < len; k++) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok && len == 3) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            ok = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0); // overlong, surrogates
        }
        if (ok && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            ok = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
        }
        if (ok) {
            out.append(s.substr(i, len));
            i += len;
        }
        else {
            out += "\xEF\xBF\xBD";
            i++;
        }
    }
    out += '"';
}

} // namespace

std::string FormatRoomListJson(const std::vector<ListedRoom>& rooms, const RoomTotals& totals, int64_t unixMs)
{
    std::string out = "{\"version\":1,\"updated\":" + std::to_string(unixMs) + ",\"totals\":{";
    out += "\"online\":" + std::to_string(totals.online) + ",\"in_public_rooms\":" + std::to_string(totals.inPublicRooms) +
           ",\"public_rooms\":" + std::to_string(totals.publicRooms) + "},\"rooms\":[";
    for (size_t i = 0; i < rooms.size(); i++) {
        const ListedRoom& r = rooms[i];
        if (i) out += ',';
        out += "{\"code\":";
        JsonString(out, r.code);
        out += ",\"title\":";
        if (const std::string text = TitleText(r.title); text.empty()) out += "null";
        else JsonString(out, text);
        out += ",\"title_id\":" + std::to_string(r.title.id) + ",\"title_characters\":[";
        for (int k = 0; k < TitleCharacters(r.title.id); k++) {
            if (k) out += ',';
            JsonString(out, r.title.chara[k]);
        }
        out += "]";
        out += ",\"owner\":";
        JsonString(out, r.owner);
        out += ",\"members\":" + std::to_string(r.members) + ",\"capacity\":" + std::to_string(kRoomCap) +
               ",\"tables\":" + std::to_string(r.tables) + ",\"open_seats\":" + std::to_string(r.openSeats) + ",\"rules\":";
        JsonString(out, r.rules);
        out += ",\"faces\":[";
        for (size_t f = 0; f < r.faces.size(); f++) {
            if (f) out += ',';
            JsonString(out, r.faces[f]);
        }
        out += "],\"players\":[";
        auto orNull = [&](const std::string& s) {
            if (s.empty()) out += "null";
            else JsonString(out, s);
        };
        for (size_t p = 0; p < r.players.size(); p++) {
            if (p) out += ',';
            out += "{\"name\":";
            JsonString(out, r.players[p].name);
            out += ",\"country\":";
            orNull(r.players[p].country);
            out += ",\"face\":";
            orNull(r.players[p].face);
            out += '}';
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

size_t Utf8Length(std::string_view text)
{
    size_t n = 0;
    for (char ch : text) n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
    return n;
}

bool ChatLimiter::Allow(int64_t nowMs, std::string_view text)
{
    if (lastMs_ >= 0 && nowMs > lastMs_) tokens_ = std::min<double>(kBurst, tokens_ + double(nowMs - lastMs_) / kRefillMs);
    lastMs_ = std::max(lastMs_, nowMs);
    const bool ok = !text.empty() && Utf8Length(text) <= kChatMaxChars && tokens_ >= 1.0;
    if (ok) tokens_ -= 1.0;
    else strikes_.push_back(nowMs);
    std::erase_if(strikes_, [&](int64_t t) { return nowMs - t >= kStrikeWindowMs; });
    return ok;
}

int ChatLimiter::Strikes(int64_t nowMs) const
{
    return static_cast<int>(std::count_if(strikes_.begin(), strikes_.end(),
                                          [&](int64_t t) { return nowMs - t < kStrikeWindowMs; }));
}

Request PeekRequest(uint32_t seq, std::string_view code) { return {seq, "peek", {std::string(code)}}; }

std::vector<std::string> PackRoomDetail(const RoomDetail& detail, size_t partBytes)
{
    // Control bytes never travel inside a field (they separate them).
    auto clean = [](std::string_view s) {
        std::string out;
        for (char ch : s) {
            if (static_cast<unsigned char>(ch) >= 0x20) out += ch;
        }
        return out;
    };
    std::vector<std::string> records;
    for (const std::string& t : detail.tables) records.push_back("t" + clean(t));
    for (const RoomDetail::Person& p : detail.people) records.push_back("p" + clean(p.chara) + "\x1F" + CleanCountry(p.country) + "\x1F" + clean(p.name));
    std::vector<std::string> bodies;
    std::string body;
    for (const std::string& r : records) {
        if (!body.empty() && body.size() + 1 + r.size() > partBytes) {
            bodies.push_back(std::move(body));
            body.clear();
        }
        if (!body.empty()) body += '\x1E';
        body += r;
    }
    bodies.push_back(std::move(body)); // an empty room still answers once
    std::vector<std::string> parts;
    for (size_t i = 0; i < bodies.size(); i++)
        parts.push_back(std::to_string(i + 1) + "/" + std::to_string(bodies.size()) + "\x1D" + bodies[i]);
    return parts;
}

bool ReadDetailPart(std::string_view text, int* part, int* parts, std::string* records)
{
    const size_t gs = text.find('\x1D'), slash = text.find('/');
    if (gs == std::string_view::npos || slash == std::string_view::npos || slash > gs) return false;
    if (!Int(text.substr(0, slash), part) || !Int(text.substr(slash + 1, gs - slash - 1), parts)) return false;
    if (*parts < 1 || *parts > 64 || *part < 1 || *part > *parts) return false;
    *records = std::string(text.substr(gs + 1));
    return true;
}

bool JoinRoomDetail(const std::vector<std::string>& records, RoomDetail* out)
{
    RoomDetail d;
    for (const std::string& body : records) {
        if (body.empty()) continue;
        for (std::string_view r : Split(body, '\x1E')) {
            if (r.empty()) return false;
            if (r[0] == 't') d.tables.emplace_back(r.substr(1));
            else if (r[0] == 'p') {
                const size_t us = r.find('\x1F'), us2 = us == std::string_view::npos ? us : r.find('\x1F', us + 1);
                if (us2 == std::string_view::npos) return false;
                d.people.push_back({std::string(r.substr(us2 + 1)), std::string(r.substr(1, us - 1)),
                                    CleanCountry(r.substr(us + 1, us2 - us - 1))});
            }
            else return false;
        }
    }
    d.code = out->code;
    *out = std::move(d);
    return true;
}

std::string FoldName(std::string_view name)
{
    std::string out;
    for (char ch : name) {
        char c = ch;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        switch (c) {
        case '0': c = 'o'; break;
        case '1': c = 'i'; break;
        case '3': c = 'e'; break;
        case '4': c = 'a'; break;
        case '5': c = 's'; break;
        case '7': c = 't'; break;
        case '@': c = 'a'; break;
        case '$': c = 's'; break;
        default: break;
        }
        if (c >= 'a' && c <= 'z') out += c;
    }
    return out;
}

std::string WordEntry(std::string_view line)
{
    std::string_view s = line;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    const bool prefix = !s.empty() && s.back() == '*';
    if (prefix) s.remove_suffix(1);
    // A phrase keeps where its words break ('_'): chat matches it only as
    // those words in a row, never letters typed apart ("b a d").
    std::string w;
    for (std::string_view word : Split(s, ' ')) {
        const std::string f = FoldName(word);
        if (f.empty()) continue;
        if (!w.empty()) w += '_';
        w += f;
    }
    if (w.empty()) return {};
    return prefix ? w + '*' : w;
}

std::vector<std::string> ParseWordFile(std::string_view text)
{
    std::vector<std::string> words;
    for (std::string_view line : Split(text, '\n')) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos || line[start] == '#') continue;
        std::string w = WordEntry(line.substr(start));
        if (!w.empty() && std::find(words.begin(), words.end(), w) == words.end()) words.push_back(std::move(w));
    }
    return words;
}

namespace {

// What folding keeps (letters, and the digits and symbols that stand for them).
bool FoldKeeps(char ch)
{
    const std::string one(1, ch);
    return !FoldName(one).empty();
}

// A chat word ends at whitespace or ASCII punctuation (not @ $ ' which are
// part of words: "a$$", "don't").
bool ChatSeparator(unsigned char c)
{
    if (c >= 0x80) return false;
    if (c <= ' ') return true;
    if (c == '@' || c == '$' || c == '\'') return false;
    return !std::isalnum(c);
}

} // namespace

bool NameRejected(std::string_view name, const std::vector<std::string>& words)
{
    const std::string folded = FoldName(name);
    if (folded.empty()) return false;
    std::vector<std::string> parts; // the name's words, folded
    std::string part;
    for (char ch : name) {
        if (FoldKeeps(ch)) part += FoldName(std::string(1, ch));
        else if (!part.empty()) parts.push_back(std::move(part)), part.clear();
    }
    if (!part.empty()) parts.push_back(std::move(part));
    for (const std::string& entry : words) {
        const bool prefix = !entry.empty() && entry.back() == '*';
        std::string w = prefix ? entry.substr(0, entry.size() - 1) : entry;
        std::erase(w, '_'); // a phrase: its words run together, as a name has them
        if (w.empty()) continue;
        if (w.size() >= 5) {
            if (folded.find(w) != std::string::npos) return true;
            continue;
        }
        for (const std::string& p : parts) {
            if (p == w || (prefix && p.rfind(w, 0) == 0)) return true;
        }
    }
    return false;
}

std::string CensorChat(std::string_view text, const std::vector<std::string>& words)
{
    if (words.empty()) return std::string(text);
    struct Word {
        size_t from, to; // bytes of the text
        std::string folded;
    };
    std::vector<Word> list;
    for (size_t i = 0; i < text.size();) {
        if (ChatSeparator(static_cast<unsigned char>(text[i]))) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < text.size() && !ChatSeparator(static_cast<unsigned char>(text[j]))) j++;
        list.push_back({i, j, FoldName(text.substr(i, j - i))});
        i = j;
    }
    std::vector<std::string_view> prefixes;
    for (const std::string& e : words) {
        if (e.size() > 1 && e.back() == '*') prefixes.emplace_back(e.data(), e.size() - 1);
    }
    auto exact = [&](const std::string& s) { return std::find(words.begin(), words.end(), s) != words.end(); };
    std::vector<bool> hit(list.size(), false);
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].folded.empty()) continue;
        // The longest run first: a phrase (its words in a row), then the word alone.
        std::string run;
        size_t longest = 0;
        for (size_t n = 1; n <= 6 && i + n <= list.size(); n++) {
            if (list[i + n - 1].folded.empty()) break;
            if (n > 1) run += '_';
            run += list[i + n - 1].folded;
            if (exact(run)) longest = n;
        }
        if (!longest) {
            for (std::string_view p : prefixes) {
                if (list[i].folded.rfind(p, 0) == 0) longest = 1;
            }
        }
        for (size_t n = 0; n < longest; n++) hit[i + n] = true;
        if (longest) i += longest - 1;
    }
    std::string out;
    size_t at = 0;
    for (size_t k = 0; k < list.size(); k++) {
        if (!hit[k]) continue;
        out.append(text.substr(at, list[k].from - at));
        for (size_t b = list[k].from; b < list[k].to; b++) {
            if ((static_cast<unsigned char>(text[b]) & 0xC0) != 0x80) out += '*'; // one per character
        }
        at = list[k].to;
    }
    out.append(text.substr(at));
    return out;
}

std::string WordListKey(int index) { return "wl" + std::to_string(index); }

std::vector<std::string> PackWordList(const std::vector<std::string>& words)
{
    std::vector<std::string> values;
    std::string current;
    for (const std::string& w : words) {
        if (w.empty() || w.size() > 500) continue;
        if (!current.empty() && current.size() + 1 + w.size() > 512) {
            values.push_back(std::move(current));
            current.clear();
            if (static_cast<int>(values.size()) == kWordListKeys) return values;
        }
        if (!current.empty()) current += ',';
        current += w;
    }
    if (!current.empty()) values.push_back(std::move(current));
    return values;
}

std::vector<std::string> UnpackWordList(const std::vector<std::string>& values)
{
    std::vector<std::string> words;
    for (const std::string& v : values) {
        for (std::string_view w : Split(v, ',')) {
            if (!w.empty()) words.emplace_back(w);
        }
    }
    return words;
}

std::string WordListHash(const std::vector<std::string>& words)
{
    uint64_t h = 1469598103934665603ull; // FNV-1a
    for (const std::string& w : words) {
        for (char c : w) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        h = (h ^ ',') * 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return hex;
}

std::string FormatMask(const std::map<uint32_t, int>& numbers)
{
    std::string s;
    for (const auto& [id, n] : numbers) {
        if (!s.empty()) s += ',';
        s += std::to_string(id) + "=" + std::to_string(n);
    }
    return s;
}

std::map<uint32_t, int> ParseMask(std::string_view text)
{
    std::map<uint32_t, int> numbers;
    if (text.empty()) return numbers;
    for (std::string_view part : Split(text, ',')) {
        const size_t eq = part.find('=');
        int64_t id = 0;
        int n = 0;
        if (eq == std::string_view::npos || !Number(part.substr(0, eq), &id) || !Int(part.substr(eq + 1), &n)) continue;
        if (id > 0 && id <= UINT32_MAX && n > 0 && n <= 9999) numbers[static_cast<uint32_t>(id)] = n;
    }
    return numbers;
}

void FormatPing(bool pong, uint64_t clockUs, uint8_t (&out)[kPingSize])
{
    out[0] = 'S';
    out[1] = 'F';
    out[2] = 'V';
    out[3] = 'P';
    out[4] = pong ? 2 : 1;
    for (int i = 0; i < 8; i++) out[5 + i] = static_cast<uint8_t>(clockUs >> (8 * i));
}

bool ParsePing(const uint8_t* data, size_t size, bool* pong, uint64_t* clockUs)
{
    if (size != kPingSize || data[0] != 'S' || data[1] != 'F' || data[2] != 'V' || data[3] != 'P' || (data[4] != 1 && data[4] != 2))
        return false;
    *pong = data[4] == 2;
    *clockUs = 0;
    for (int i = 0; i < 8; i++) *clockUs |= uint64_t(data[5 + i]) << (8 * i);
    return true;
}

std::string StateKey(int table) { return "s" + std::to_string(table); }
std::string QueueKey(int table) { return "q" + std::to_string(table); }

std::string FormatIds(const std::vector<uint32_t>& ids)
{
    std::string s;
    for (uint32_t id : ids) {
        if (!s.empty()) s += ',';
        s += std::to_string(id);
    }
    return s;
}

std::vector<uint32_t> ParseIds(std::string_view text)
{
    std::vector<uint32_t> ids;
    if (text.empty()) return ids;
    for (std::string_view part : Split(text, ',')) {
        int64_t n = 0;
        if (Number(part, &n) && n > 0 && n <= UINT32_MAX) ids.push_back(static_cast<uint32_t>(n));
    }
    return ids;
}

// "<phase>|<p1>|<p2>|<ready bits>|<w1>|<w2>|<code>|<stayer>|<streak>|<last winner>|<last loser>|<lw>|<ll>"
std::string FormatTable(const TableState& s)
{
    return std::string(1, static_cast<char>(s.phase)) + "|" + std::to_string(s.seat[0]) + "|" + std::to_string(s.seat[1]) + "|" +
           std::to_string((s.ready[0] ? 1 : 0) | (s.ready[1] ? 2 : 0)) + "|" + std::to_string(s.wins[0]) + "|" +
           std::to_string(s.wins[1]) + "|" + s.code + "|" + std::to_string(s.stayer) + "|" + std::to_string(s.streak) + "|" +
           std::to_string(s.lastWinner) + "|" + std::to_string(s.lastLoser) + "|" + std::to_string(s.lastScore[0]) + "|" +
           std::to_string(s.lastScore[1]) + "|" + std::to_string(s.call) + "|" + std::to_string(s.callSeconds) + "|" + s.watch + "|" + (s.again ? "1" : "0");
}

bool ParseTable(std::string_view text, TableState* out)
{
    const auto f = Split(text, '|');
    // 17 since playing on (again), 16 since the relay (its room), 15 before.
    if (f.size() < 15 || f.size() > 17 || f[0].size() != 1) return false;
    TableState s;
    const char p = f[0][0];
    if (p != 'o' && p != 'c' && p != 'n' && p != 'p' && p != 'a') return false;
    s.phase = static_cast<TablePhase>(p);
    int64_t n[14] = {};
    for (int i = 1; i < 15; i++) {
        if (i == 6) continue; // the code
        if (!Number(f[i], &n[i - 1]) || n[i - 1] < 0 || n[i - 1] > UINT32_MAX) return false;
    }
    s.seat[0] = static_cast<uint32_t>(n[0]);
    s.seat[1] = static_cast<uint32_t>(n[1]);
    s.ready[0] = n[2] & 1;
    s.ready[1] = n[2] & 2;
    s.wins[0] = static_cast<uint8_t>(std::min<int64_t>(n[3], 99));
    s.wins[1] = static_cast<uint8_t>(std::min<int64_t>(n[4], 99));
    s.code = std::string(f[6]);
    s.stayer = static_cast<uint32_t>(n[6]);
    s.streak = static_cast<int>(std::min<int64_t>(n[7], 999));
    s.lastWinner = static_cast<uint32_t>(n[8]);
    s.lastLoser = static_cast<uint32_t>(n[9]);
    s.lastScore[0] = static_cast<uint8_t>(std::min<int64_t>(n[10], 99));
    s.lastScore[1] = static_cast<uint8_t>(std::min<int64_t>(n[11], 99));
    s.call = static_cast<uint32_t>(n[12]);
    s.callSeconds = static_cast<uint8_t>(std::min<int64_t>(n[13], 99));
    if (f.size() >= 16) s.watch = std::string(f[15]);
    if (f.size() >= 17) s.again = f[16] == "1";
    *out = std::move(s);
    return true;
}

void FillSeats(TableState* s, std::vector<uint32_t>* queue)
{
    for (int i = 0; i < 2; i++) {
        if (s->seat[i] || queue->empty()) continue;
        s->seat[i] = queue->front();
        queue->erase(queue->begin());
    }
    s->ready[0] = s->ready[1] = false;
    s->wins[0] = s->wins[1] = 0;
    s->code.clear();
    s->phase = s->seat[0] && s->seat[1] ? TablePhase::Call : TablePhase::Open;
}

void ApplySetResult(const Rules& rules, int winnerSeat, const uint8_t (&games)[2], TableState* s, std::vector<uint32_t>* queue)
{
    const int w = winnerSeat == 1 ? 1 : 0;
    const uint32_t winner = s->seat[w], loser = s->seat[1 - w];
    // The set before this one: who played it as well has now played two in
    // a row.
    const uint32_t before[2] = {s->lastWinner, s->lastLoser};
    auto playedBefore = [&](uint32_t id) { return id && (id == before[0] || id == before[1]); };
    s->lastWinner = winner;
    s->lastLoser = loser;
    s->lastScore[0] = games[w];
    s->lastScore[1] = games[1 - w];
    // Who may stay: by the rotation, while they have played fewer than the
    // limit's sets in a row here.
    uint32_t stays = 0;
    if (rules.rotation == Rotation::WinnerStays) stays = winner;
    else if (rules.rotation == Rotation::LoserStays) stays = loser;
    const int played = stays && stays == s->stayer ? s->streak + 1 : 1; // sets in a row, this one counted
    if (stays && rules.staysLimit && played >= rules.staysLimit) stays = 0; // done: they go too
    auto toBack = [&](uint32_t id) {
        if (!id) return;
        std::erase(*queue, id);
        queue->push_back(id);
    };
    for (int i = 0; i < 2; i++) {
        if (s->seat[i] && s->seat[i] != stays) s->seat[i] = 0;
    }
    // Both go back (round robin, or a stayer at the limit): who has played
    // two sets in a row waits longest; otherwise the loser goes first. So
    // three players at a round robin table take turns whoever wins.
    const bool winnerFirst = playedBefore(loser) && !playedBefore(winner);
    if (winnerFirst) {
        if (winner != stays) toBack(winner);
        if (loser != stays) toBack(loser);
    }
    else {
        if (loser != stays) toBack(loser);
        if (winner != stays) toBack(winner);
    }
    s->stayer = stays;
    s->streak = stays ? played : 0;
    s->ready[0] = s->ready[1] = false;
    s->wins[0] = s->wins[1] = 0;
    s->code.clear();
    s->phase = TablePhase::After;
}

bool LeaveTable(uint32_t id, TableState* s, std::vector<uint32_t>* queue)
{
    bool was = false;
    for (int i = 0; i < 2; i++) {
        if (s->seat[i] == id) {
            s->seat[i] = 0;
            s->ready[i] = false;
            was = true;
        }
    }
    const size_t before = queue->size();
    std::erase(*queue, id);
    was |= queue->size() != before;
    if (s->stayer == id) {
        s->stayer = 0;
        s->streak = 0;
    }
    return was;
}

std::string FormatTicket(const TableTicket& t)
{
    char token[17];
    std::snprintf(token, sizeof(token), "%016llx", static_cast<unsigned long long>(t.token));
    if (t.host) return "host|" + std::to_string(t.table) + "|" + token + "|" + t.rules;
    return "join|" + std::to_string(t.table) + "|" + t.code + "|" + token + "|" + t.rules;
}

bool ParseTicket(std::string_view text, TableTicket* out)
{
    const auto f = Split(text, '|');
    TableTicket t;
    if (f.size() == 4 && f[0] == "host") t.host = true;
    else if (f.size() == 5 && f[0] == "join") t.host = false;
    else return false;
    if (!Int(f[1], &t.table) || t.table < 0 || t.table >= kMaxTables) return false;
    const std::string_view token = t.host ? f[2] : f[3];
    if (token.size() != 16) return false;
    const auto [end, ec] = std::from_chars(token.data(), token.data() + 16, t.token, 16);
    if (ec != std::errc() || end != token.data() + 16) return false;
    if (!t.host) t.code = std::string(f[2]);
    t.rules = std::string(t.host ? f[3] : f[4]);
    Rules rules;
    if (!ParseRules(t.rules, &rules)) return false;
    *out = std::move(t);
    return true;
}

uint32_t NextOwner(const std::vector<uint32_t>& joinOrder, uint32_t leaving, uint32_t keeper)
{
    for (uint32_t id : joinOrder) {
        if (id != leaving && id != keeper && id != 0) return id;
    }
    return 0;
}

} // namespace sfv::keeper
