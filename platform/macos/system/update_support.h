// In-app updates, macOS pieces without networking or AppKit (unit-tested in mac_unit):
// signature check with the Security framework, SHA-256 with CommonCrypto, "can this copy of
// the app be replaced in place?" and the text of the bundle-swap helper script.
// The portable logic (versions, release JSON, URL allow-list, SHA256SUMS) is core/update/update.h.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace snow::mac::updates {

// ECDSA P-256 / SHA-256 over `data`, `derSig` as written by `openssl dgst -sha256 -sign` (DER,
// passed to SecKeyVerifySignature as is). `publicKey` = uncompressed point 04 || X || Y
// (default: update::kReleasePublicKey).
bool verifySignature(const void* data, std::size_t size, const void* derSig, std::size_t sigSize,
                     const std::uint8_t* publicKey = nullptr, std::size_t keySize = 65);

std::string sha256Hex(const void* data, std::size_t size);   // lower-case hex
bool sha256FileHex(const std::string& path, std::string& hex);

// What the OS says about the place the running bundle lives in (gathered by bundleFacts()).
struct BundleFacts {
    bool exists = false;
    bool readOnlyVolume = false;   // MNT_RDONLY (a mounted .dmg, a locked volume)
    bool ejectableVolume = false;  // disk images and removable drives
    bool parentWritable = false;   // the user may rename entries of the parent directory
};

// App Translocation runs quarantined apps from a random read-only path
// (/private/var/folders/…/AppTranslocation/<uuid>/d/SnowFall.app).
bool isTranslocatedPath(std::string_view bundlePath);
// "" if the bundle can be replaced in place, otherwise a short reason for the log / diagnostics:
// "not-a-bundle", "translocated", "read-only-volume", "disk-image", "not-writable".
std::string installBlocker(std::string_view bundlePath, const BundleFacts& facts);
BundleFacts bundleFacts(const std::string& bundlePath);

std::string parentDirectory(std::string_view path);   // "/Applications/SnowFall.app" -> "/Applications"
// Staging and backup names next to the bundle (same volume, so the swap is two renames).
std::string stagingBundlePath(std::string_view bundlePath);   // <parent>/.SnowFall-update.app
std::string backupBundlePath(std::string_view bundlePath);    // <parent>/.SnowFall-old.app

// /bin/sh script that swaps the bundles after the app has quit. Arguments:
//   $1 pid of the app   $2 current .app   $3 staged .app   $4 backup path   $5 its own temp dir
//   $6 log file         $7 "1" = open the app afterwards (the self-test passes "0")
// Waits up to 60 s for the pid to exit, keeps the old bundle until the new one is in place
// (rolls back on failure), removes the backup, re-registers and opens the app.
const char* swapHelperScript();

// "/Users/name/Library/..." -> "~/Library/..." for log lines (no user names in the log).
std::string redactHome(std::string text);

}  // namespace snow::mac::updates
