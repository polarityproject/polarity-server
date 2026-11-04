#include "keeper/sign.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#else
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#endif

namespace sfv::keeper {

std::string Hex(const uint8_t* data, size_t size)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < size; i++) {
        s += kDigits[data[i] >> 4];
        s += kDigits[data[i] & 15];
    }
    return s;
}

bool FromHex(std::string_view text, uint8_t* out, size_t size)
{
    if (text.size() != size * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < size; i++) {
        const int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

std::string KeyToText(const KeyPair& key) { return Hex(key.d, 32) + Hex(key.pub, 64); }

bool KeyFromText(std::string_view text, KeyPair* out)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    return text.size() == 192 && FromHex(text.substr(0, 64), out->d, 32) && FromHex(text.substr(64), out->pub, 64);
}

#ifdef _WIN32

bool GenerateKey(KeyPair* out)
{
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, &key, 256, 0))) return false;
    bool ok = BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0));
    // BCRYPT_ECCKEY_BLOB, then x, y and d.
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96] = {};
    ULONG size = 0;
    ok = ok && BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof(blob), &size, 0)) &&
         size == sizeof(blob);
    BCryptDestroyKey(key);
    if (!ok) return false;
    std::memcpy(out->pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    std::memcpy(out->d, blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64, 32);
    return true;
}

bool Sign(const KeyPair& pair, const uint8_t* data, size_t size, uint8_t (&signature)[64])
{
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 96] = {};
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    header->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), pair.pub, 64);
    std::memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64, pair.d, 32);
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, blob,
                                            sizeof(blob), 0))) {
        return false;
    }
    uint8_t digest[32] = {};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<uint8_t*>(data), static_cast<ULONG>(size), digest,
               sizeof(digest));
    ULONG written = 0;
    const bool ok = BCRYPT_SUCCESS(BCryptSignHash(key, nullptr, digest, sizeof(digest), signature, 64, &written, 0)) &&
                    written == 64;
    BCryptDestroyKey(key);
    return ok;
}

#else

bool GenerateKey(KeyPair* out)
{
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    if (!key) return false;
    BIGNUM* d = nullptr;
    uint8_t pub[65] = {};
    size_t pubSize = 0;
    const bool ok = EVP_PKEY_get_bn_param(key, OSSL_PKEY_PARAM_PRIV_KEY, &d) &&
                    EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_PUB_KEY, pub, sizeof(pub), &pubSize) &&
                    pubSize == 65 && pub[0] == 0x04 && BN_bn2binpad(d, out->d, 32) == 32;
    if (ok) std::memcpy(out->pub, pub + 1, 64);
    BN_clear_free(d);
    EVP_PKEY_free(key);
    return ok;
}

bool Sign(const KeyPair& pair, const uint8_t* data, size_t size, uint8_t (&signature)[64])
{
    uint8_t pub[65];
    pub[0] = 0x04;
    std::memcpy(pub + 1, pair.pub, 64);
    BIGNUM* d = BN_bin2bn(pair.d, 32, nullptr);
    OSSL_PARAM_BLD* build = OSSL_PARAM_BLD_new();
    OSSL_PARAM* params = nullptr;
    EVP_PKEY_CTX* fromData = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    EVP_PKEY* key = nullptr;
    bool ok = d && build && fromData &&
              OSSL_PARAM_BLD_push_utf8_string(build, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) &&
              OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_PRIV_KEY, d) &&
              OSSL_PARAM_BLD_push_octet_string(build, OSSL_PKEY_PARAM_PUB_KEY, pub, sizeof(pub)) &&
              (params = OSSL_PARAM_BLD_to_param(build)) != nullptr && EVP_PKEY_fromdata_init(fromData) > 0 &&
              EVP_PKEY_fromdata(fromData, &key, EVP_PKEY_KEYPAIR, params) > 0;
    uint8_t der[80] = {};
    size_t derSize = sizeof(der);
    EVP_MD_CTX* md = ok ? EVP_MD_CTX_new() : nullptr;
    ok = ok && md && EVP_DigestSignInit(md, nullptr, EVP_sha256(), nullptr, key) > 0 &&
         EVP_DigestSign(md, der, &derSize, data, size) > 0;
    if (ok) {
        // DER ECDSA-Sig-Value to r and s, 32 bytes each.
        const uint8_t* p = der;
        ECDSA_SIG* sig = d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(derSize));
        ok = sig && BN_bn2binpad(ECDSA_SIG_get0_r(sig), signature, 32) == 32 &&
             BN_bn2binpad(ECDSA_SIG_get0_s(sig), signature + 32, 32) == 32;
        ECDSA_SIG_free(sig);
    }
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(fromData);
    OSSL_PARAM_free(params);
    OSSL_PARAM_BLD_free(build);
    BN_clear_free(d);
    return ok;
}

#endif

} // namespace sfv::keeper
