#include "update/crypto_win.h"

#include <bcrypt.h>

#include <cstring>

#include "update/update.h"

#pragma comment(lib, "bcrypt.lib")

namespace snow::update {

namespace {
bool succeeded(NTSTATUS s) { return s >= 0; }
}  // namespace

Sha256::Sha256() {
    BCRYPT_HASH_HANDLE h = nullptr;
    if (succeeded(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &h, nullptr, 0, nullptr, 0, 0))) hash_ = h;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
}

void Sha256::update(const void* data, std::size_t size) {
    if (!ok()) return;
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (size > 0) {   // BCryptHashData takes a ULONG length
        const ULONG n = size > 0x40000000u ? 0x40000000u : static_cast<ULONG>(size);
        if (!succeeded(BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), const_cast<PUCHAR>(p), n, 0))) {
            failed_ = true;
            return;
        }
        p += n;
        size -= n;
    }
}

bool Sha256::finish(std::uint8_t out[32]) {
    if (!ok()) return false;
    const bool r = succeeded(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), out, 32, 0));
    failed_ = true;   // a finished hash object cannot be reused
    return r;
}

bool Sha256::digest(const void* data, std::size_t size, std::uint8_t out[32]) {
    Sha256 h;
    h.update(data, size);
    return h.finish(out);
}

bool verifySignature(std::string_view data, const std::vector<std::uint8_t>& derSignature,
                     const std::uint8_t publicKey[65]) {
    if (!publicKey || publicKey[0] != 0x04) return false;
    std::uint8_t raw[64];
    if (!derSignatureToRaw(derSignature, raw)) return false;
    std::uint8_t digest[32];
    if (!Sha256::digest(data.data(), data.size(), digest)) return false;

    struct {
        BCRYPT_ECCKEY_BLOB header;
        std::uint8_t xy[64];
    } blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    std::memcpy(blob.xy, publicKey + 1, 64);

    BCRYPT_KEY_HANDLE key = nullptr;
    if (!succeeded(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                       reinterpret_cast<PUCHAR>(&blob), sizeof blob, 0)))
        return false;
    const NTSTATUS s = BCryptVerifySignature(key, nullptr, digest, sizeof digest, raw, sizeof raw, 0);
    BCryptDestroyKey(key);
    return succeeded(s);
}

bool verifyReleaseSignature(std::string_view data, const std::vector<std::uint8_t>& derSignature) {
    return verifySignature(data, derSignature, kReleasePublicKey);
}

}  // namespace snow::update
