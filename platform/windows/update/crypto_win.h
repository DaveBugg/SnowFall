// SHA-256 and the release-signature check through Windows CNG (bcrypt). No other crypto code.
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace snow::update {

// Incremental SHA-256 (BCrypt). ok() is false when CNG could not create the hash object.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool ok() const { return hash_ != nullptr && !failed_; }
    void update(const void* data, std::size_t size);
    // Writes the digest; the object cannot be used afterwards.
    bool finish(std::uint8_t out[32]);

    // One-shot helper.
    static bool digest(const void* data, std::size_t size, std::uint8_t out[32]);

private:
    void* hash_ = nullptr;   // BCRYPT_HASH_HANDLE
    bool failed_ = false;
};

// ECDSA P-256 / SHA-256 signature of `data` (DER, as written by `openssl dgst -sha256 -sign`)
// checked against an uncompressed public key 04 || X || Y (default: the release key).
bool verifySignature(std::string_view data, const std::vector<std::uint8_t>& derSignature,
                     const std::uint8_t publicKey[65]);
bool verifyReleaseSignature(std::string_view data, const std::vector<std::uint8_t>& derSignature);

}  // namespace snow::update
