// The word filter: folding, the word file, rejected names, censored chat, the word list and masks.

#include "core/keeper_protocol.h"
#include "core/keeper_protocol_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace sfv::keeper {

using namespace detail;

namespace {

constexpr size_t kAnywhereLength = 5; // entries this long match anywhere in a name
constexpr size_t kPhraseWords = 6;
constexpr size_t kEntryMax = 500, kValueBytes = 512;
constexpr uint64_t kFnvOffset = 1469598103934665603ull, kFnvPrime = 1099511628211ull;
constexpr int kMaskMax = 9999;

} // namespace

std::string FoldName(std::string_view name) {
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

std::string WordEntry(std::string_view line) {
    std::string_view s = line;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    const bool prefix = !s.empty() && s.back() == '*';
    if (prefix) s.remove_suffix(1);
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

std::vector<std::string> ParseWordFile(std::string_view text) {
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

bool FoldKeeps(char ch) {
    const std::string one(1, ch);
    return !FoldName(one).empty();
}

bool ChatSeparator(unsigned char c) {
    if (c >= 0x80) return false;
    if (c <= ' ') return true;
    if (c == '@' || c == '$' || c == '\'') return false;
    return !std::isalnum(c);
}

} // namespace

bool NameRejected(std::string_view name, const std::vector<std::string>& words) {
    const std::string folded = FoldName(name);
    if (folded.empty()) return false;
    std::vector<std::string> parts;
    std::string part;
    for (char ch : name) {
        if (FoldKeeps(ch)) part += FoldName(std::string(1, ch));
        else if (!part.empty()) parts.push_back(std::move(part)), part.clear();
    }
    if (!part.empty()) parts.push_back(std::move(part));
    for (const std::string& entry : words) {
        const bool prefix = !entry.empty() && entry.back() == '*';
        std::string w = prefix ? entry.substr(0, entry.size() - 1) : entry;
        std::erase(w, '_');
        if (w.empty()) continue;
        if (w.size() >= kAnywhereLength) {
            if (folded.find(w) != std::string::npos) return true;
            continue;
        }
        for (const std::string& p : parts) {
            if (p == w || (prefix && p.rfind(w, 0) == 0)) return true;
        }
    }
    return false;
}

std::string CensorChat(std::string_view text, const std::vector<std::string>& words) {
    if (words.empty()) return std::string(text);

    struct Word {
        size_t from, to;
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
        std::string run;
        size_t longest = 0;
        for (size_t n = 1; n <= kPhraseWords && i + n <= list.size(); n++) {
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
            if ((static_cast<unsigned char>(text[b]) & 0xC0) != 0x80) out += '*';
        }
        at = list[k].to;
    }
    out.append(text.substr(at));
    return out;
}

std::string WordListKey(int index) { return "wl" + std::to_string(index); }

std::vector<std::string> PackWordList(const std::vector<std::string>& words) {
    std::vector<std::string> values;
    std::string current;
    for (const std::string& w : words) {
        if (w.empty() || w.size() > kEntryMax) continue;
        if (!current.empty() && current.size() + 1 + w.size() > kValueBytes) {
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

std::vector<std::string> UnpackWordList(const std::vector<std::string>& values) {
    std::vector<std::string> words;
    for (const std::string& v : values) {
        for (std::string_view w : Split(v, ',')) {
            if (!w.empty()) words.emplace_back(w);
        }
    }
    return words;
}

std::string WordListHash(const std::vector<std::string>& words) {
    uint64_t h = kFnvOffset;
    for (const std::string& w : words) {
        for (char c : w) h = (h ^ static_cast<unsigned char>(c)) * kFnvPrime;
        h = (h ^ ',') * kFnvPrime;
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return hex;
}

std::string FormatMask(const std::map<uint32_t, int>& numbers) {
    std::string s;
    for (const auto& [id, n] : numbers) {
        if (!s.empty()) s += ',';
        s += std::to_string(id) + "=" + std::to_string(n);
    }
    return s;
}

std::map<uint32_t, int> ParseMask(std::string_view text) {
    std::map<uint32_t, int> numbers;
    if (text.empty()) return numbers;
    for (std::string_view part : Split(text, ',')) {
        const size_t eq = part.find('=');
        int64_t id = 0;
        int n = 0;
        if (eq == std::string_view::npos || !Number(part.substr(0, eq), &id) || !Int(part.substr(eq + 1), &n)) continue;
        if (id > 0 && id <= UINT32_MAX && n > 0 && n <= kMaskMax) numbers[static_cast<uint32_t>(id)] = n;
    }
    return numbers;
}

} // namespace sfv::keeper
