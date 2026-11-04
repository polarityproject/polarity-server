#pragma once
// The keeper's signing key (ECDSA P-256, as the updater's): it signs its proof
// (keeper_protocol.h ProofMessage), which the mod checks with the public half
// (sfv::VerifySignature). BCrypt on Windows, OpenSSL on Linux.
//
// Key file: one line of hex, the private scalar d then the public x and y
// (96 bytes, 192 hex digits). `sfvr-keeper --new-key <file>` writes one and
// prints the public half for the mod.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sfv::keeper {

struct KeyPair {
    uint8_t d[32] = {};
    uint8_t pub[64] = {}; // x then y, as the mod's public keys
};

bool GenerateKey(KeyPair* out);
// 64 bytes, r then s, over SHA-256(data).
bool Sign(const KeyPair& key, const uint8_t* data, size_t size, uint8_t (&signature)[64]);

std::string Hex(const uint8_t* data, size_t size);
bool FromHex(std::string_view text, uint8_t* out, size_t size);
std::string KeyToText(const KeyPair& key);
bool KeyFromText(std::string_view text, KeyPair* out);

} // namespace sfv::keeper
