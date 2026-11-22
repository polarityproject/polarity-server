#pragma once
// What keeper_protocol.cpp and keeper_words.cpp share: the field parsers.

#include <cstdint>
#include <string_view>
#include <vector>

namespace sfv::keeper::detail {

// A whole decimal field of at most 10 characters.
bool Number(std::string_view text, int64_t* out);
bool Int(std::string_view text, int* out);
// Every part between `sep`s, empty ones included.
std::vector<std::string_view> Split(std::string_view text, char sep);

} // namespace sfv::keeper::detail
