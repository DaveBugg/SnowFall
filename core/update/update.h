// Update check against GitHub Releases: the portable part (no OS headers, no networking).
//
// Privacy: the app makes at most one request a day to the GitHub API (plus the downloads the
// user starts), only when the user agreed (settings `updates.check`). Nothing about the user or
// the machine is sent: a plain GET with a "SnowFall/<version>" User-Agent.
//
// Integrity: every release carries SHA256SUMS and SHA256SUMS.sig — an ECDSA P-256 / SHA-256
// signature (DER, as written by `openssl dgst -sha256 -sign`) of the SHA256SUMS bytes, made in CI
// with the release key (secret RELEASE_SIGNING_KEY). The app verifies the signature with the
// public key below (the platform does the ECDSA math: Windows CNG, macOS Security), then the
// SHA-256 of the downloaded installer against SHA256SUMS. A release without a valid signature is
// never installed automatically: the user gets a link to the release page instead.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/settings.h"

namespace snow::update {

inline constexpr const char* kOwnerRepo = "DaveBugg/SnowFall";
inline constexpr const char* kLatestReleaseApi = "https://api.github.com/repos/DaveBugg/SnowFall/releases/latest";
inline constexpr const char* kReleasesPage = "https://github.com/DaveBugg/SnowFall/releases";
inline constexpr const char* kChecksumsName = "SHA256SUMS";
inline constexpr const char* kSignatureName = "SHA256SUMS.sig";
inline constexpr std::int64_t kCheckIntervalSeconds = 24 * 60 * 60;
inline constexpr double kStartupDelaySeconds = 30.0;   // first automatic check after start
inline constexpr std::size_t kMaxApiBytes = 512 * 1024;
inline constexpr std::size_t kMaxSmallFileBytes = 64 * 1024;   // SHA256SUMS / .sig
inline constexpr std::size_t kMaxInstallerBytes = 200 * 1024 * 1024;

// Release public key: uncompressed P-256 point 04 || X || Y.
extern const std::uint8_t kReleasePublicKey[65];

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool valid = false;
    std::string text() const;   // "0.1.2"
};
// "0.1.2", "v0.1.2", "0.1.2+abc1234", "0.1.2-beta" (suffixes ignored). Invalid → valid == false.
Version parseVersion(std::string_view s);
int compare(const Version& a, const Version& b);   // <0, 0, >0
// True when `latest` is newer than `current` (both valid).
bool isNewer(const Version& latest, const Version& current);

struct Asset {
    std::string name;
    std::string url;   // browser_download_url
    long long size = 0;
};

struct ReleaseInfo {
    std::string tag;          // "v0.1.3"
    Version version;
    std::string pageUrl;      // html_url
    std::string notes;        // body (markdown), trimmed to a few KB
    std::vector<Asset> assets;
};

// Parses the GitHub "latest release" JSON. Drafts / prereleases and releases of other
// repositories are rejected. All URLs are checked with isAllowedUrl().
bool parseLatestRelease(std::string_view json, ReleaseInfo& out, std::string* error = nullptr);

const Asset* findAsset(const ReleaseInfo& r, std::string_view name);
// The installer of this platform: "SnowFallSetup.exe" (Windows) or the first "*.dmg" (macOS).
enum class Platform { Windows, MacOS };
const Asset* installerAsset(const ReleaseInfo& r, Platform p);

// Only https URLs of this repository on github.com / api.github.com, and the GitHub download
// hosts the release assets redirect to.
bool isAllowedUrl(std::string_view url);

// "SHA256SUMS" text (`sha256sum` format: "<64 hex>  <name>" or "<hex> *<name>") → name → hex
// (lower case). Malformed lines are ignored.
std::map<std::string, std::string> parseChecksums(std::string_view text);

std::string toHex(const std::uint8_t* data, std::size_t n);
// DER ECDSA signature (SEQUENCE { INTEGER r, INTEGER s }) → raw r || s, 32 bytes each.
bool derSignatureToRaw(const std::vector<std::uint8_t>& der, std::uint8_t out[64]);

// Automatic check due? (user agreed, and the last one was a day ago or more / never).
bool autoCheckDue(const UpdateSettings& s, std::int64_t nowUnix);

// What the UI shows. Shared by both platforms; JSON form for the macOS bridge.
enum class Stage { Idle, Checking, UpToDate, Available, Downloading, Installing, Failed };
struct Status {
    Stage stage = Stage::Idle;
    std::string current;        // running version ("0.1.2")
    std::string latest;         // newer version when Available / Downloading / Installing
    std::string pageUrl;        // release page of `latest` (or the releases list)
    std::string notes;          // release notes of `latest`
    bool canInstall = false;    // signed release + installer asset + installable copy of the app
    int progress = 0;           // 0..100 while Downloading
    std::string error;          // short reason when Failed (UI language not applied)
    bool operator==(const Status&) const = default;
};
std::string statusToJson(const Status& s);
const char* stageName(Stage s);   // "idle", "checking", ...

}  // namespace snow::update
