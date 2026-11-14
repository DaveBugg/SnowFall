#include "update/update.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "settings/json.h"

namespace snow::update {

// Generated with `openssl ecparam -name prime256v1 -genkey`; the private key is the CI secret
// RELEASE_SIGNING_KEY (kept offline by the maintainer, never in the repository).
const std::uint8_t kReleasePublicKey[65] = {
    0x04, 0x94, 0x18, 0xf1, 0x4a, 0x60, 0xb8, 0x0d, 0xa9, 0x70, 0x74, 0x57, 0xc3, 0x2d, 0x5f, 0x06, 0xd7,
    0x71, 0x98, 0x26, 0x1a, 0x77, 0xdb, 0x04, 0xe7, 0xa9, 0xfe, 0x1b, 0x0f, 0x98, 0x19, 0x72, 0x24,
    0x1f, 0xf7, 0x3a, 0xe8, 0xf2, 0x29, 0x4b, 0xab, 0x7f, 0x9e, 0x3f, 0x27, 0x5a, 0x45, 0x57, 0x78,
    0xdf, 0xca, 0x72, 0x9d, 0x5e, 0xa3, 0x5d, 0x91, 0xf6, 0xac, 0x37, 0x67, 0x63, 0xb8, 0xee, 0xa3,
};

namespace {

bool startsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool readInt(std::string_view& s, int& out) {
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.front()))) return false;
    long v = 0;
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.front()))) {
        v = v * 10 + (s.front() - '0');
        if (v > 100000) return false;
        s.remove_prefix(1);
    }
    out = static_cast<int>(v);
    return true;
}

const std::string& str(const json::Value* v) {
    static const std::string empty;
    return v && v->isString() ? v->asString() : empty;
}

}  // namespace

std::string Version::text() const {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%d.%d.%d", major, minor, patch);
    return buf;
}

Version parseVersion(std::string_view s) {
    Version v;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.remove_prefix(1);
    if (!readInt(s, v.major) || s.empty() || s.front() != '.') return Version{};
    s.remove_prefix(1);
    if (!readInt(s, v.minor)) return Version{};
    if (!s.empty() && s.front() == '.') {
        s.remove_prefix(1);
        if (!readInt(s, v.patch)) return Version{};
    }
    if (!s.empty() && s.front() != '+' && s.front() != '-' && !std::isspace(static_cast<unsigned char>(s.front())))
        return Version{};
    v.valid = true;
    return v;
}

int compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool isNewer(const Version& latest, const Version& current) {
    return latest.valid && current.valid && compare(latest, current) > 0;
}

bool isAllowedUrl(std::string_view url) {
    if (url.find_first_of(" \t\r\n\\") != std::string_view::npos) return false;
    const std::string u = lower(url);
    if (!startsWith(u, "https://")) return false;
    std::string_view rest = std::string_view(u).substr(8);
    const std::size_t slash = rest.find('/');
    const std::string_view host = rest.substr(0, slash);
    const std::string_view path = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    if (host.find('@') != std::string_view::npos || host.find(':') != std::string_view::npos) return false;
    if (host == "github.com") return startsWith(path, "/davebugg/snowfall/");
    if (host == "api.github.com") return startsWith(path, "/repos/davebugg/snowfall/");
    // Release asset downloads redirect to these GitHub storage hosts (signed, short-lived URLs).
    return host == "objects.githubusercontent.com" || host == "release-assets.githubusercontent.com" ||
           host == "github-releases.githubusercontent.com";
}

bool parseLatestRelease(std::string_view text, ReleaseInfo& out, std::string* error) {
    auto fail = [error](const char* why) {
        if (error) *error = why;
        return false;
    };
    json::Value root;
    if (!json::parse(text, root) || !root.isObject()) return fail("bad JSON");
    if (const json::Value* d = root.find("draft"); d && d->asBool(false)) return fail("draft release");
    if (const json::Value* p = root.find("prerelease"); p && p->asBool(false)) return fail("prerelease");
    ReleaseInfo r;
    r.tag = str(root.find("tag_name"));
    r.version = parseVersion(r.tag);
    if (!r.version.valid) return fail("bad tag");
    r.pageUrl = str(root.find("html_url"));
    if (!isAllowedUrl(r.pageUrl) || lower(r.pageUrl).find("/davebugg/snowfall/releases/") == std::string::npos)
        r.pageUrl = kReleasesPage;
    r.notes = str(root.find("body"));
    if (r.notes.size() > 8000) r.notes.resize(8000);
    if (const json::Value* a = root.find("assets"); a && a->isArray()) {
        for (std::size_t i = 0; i < a->size(); ++i) {
            const json::Value& e = a->at(i);
            if (!e.isObject()) continue;
            Asset as;
            as.name = str(e.find("name"));
            as.url = str(e.find("browser_download_url"));
            if (const json::Value* sz = e.find("size"); sz && sz->isNumber())
                as.size = static_cast<long long>(sz->asNumber());
            if (as.name.empty() || as.name.find('/') != std::string::npos || as.name.find('\\') != std::string::npos ||
                !isAllowedUrl(as.url))
                continue;
            r.assets.push_back(std::move(as));
        }
    }
    out = std::move(r);
    return true;
}

const Asset* findAsset(const ReleaseInfo& r, std::string_view name) {
    for (const Asset& a : r.assets)
        if (a.name == name) return &a;
    return nullptr;
}

const Asset* installerAsset(const ReleaseInfo& r, Platform p) {
    if (p == Platform::Windows) return findAsset(r, "SnowFallSetup.exe");
    for (const Asset& a : r.assets)
        if (a.name.size() > 4 && lower(a.name).compare(a.name.size() - 4, 4, ".dmg") == 0) return &a;
    return nullptr;
}

std::map<std::string, std::string> parseChecksums(std::string_view text) {
    std::map<std::string, std::string> out;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.size() < 66) continue;
        const std::string_view hex = line.substr(0, 64);
        if (!std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
            continue;
        std::string_view name = line.substr(64);
        if (name.empty() || name.front() != ' ') continue;
        name.remove_prefix(1);
        if (!name.empty() && (name.front() == ' ' || name.front() == '*')) name.remove_prefix(1);
        if (startsWith(name, "./")) name.remove_prefix(2);
        if (name.empty()) continue;
        out[std::string(name)] = lower(hex);
    }
    return out;
}

std::string toHex(const std::uint8_t* data, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s += d[data[i] >> 4];
        s += d[data[i] & 15];
    }
    return s;
}

bool derSignatureToRaw(const std::vector<std::uint8_t>& der, std::uint8_t out[64]) {
    std::size_t i = 0;
    auto len = [&](std::size_t& n) -> bool {
        if (i >= der.size()) return false;
        std::uint8_t b = der[i++];
        if (b < 0x80) {
            n = b;
            return true;
        }
        if (b != 0x81 || i >= der.size()) return false;   // signatures are < 256 bytes
        n = der[i++];
        return true;
    };
    std::size_t seqLen = 0;
    if (der.size() < 8 || der[i++] != 0x30 || !len(seqLen) || i + seqLen != der.size()) return false;
    for (int k = 0; k < 2; ++k) {
        std::size_t n = 0;
        if (i >= der.size() || der[i++] != 0x02 || !len(n) || n == 0 || i + n > der.size()) return false;
        const std::uint8_t* p = der.data() + i;
        i += n;
        while (n > 0 && *p == 0) {   // strip the sign / leading zero bytes
            ++p;
            --n;
        }
        if (n > 32) return false;
        std::uint8_t* dst = out + 32 * k;
        std::fill(dst, dst + 32, std::uint8_t{0});
        std::copy(p, p + n, dst + (32 - n));
    }
    return i == der.size();
}

bool autoCheckDue(const UpdateSettings& s, std::int64_t nowUnix) {
    if (!s.check) return false;
    if (s.lastCheckUnix <= 0 || s.lastCheckUnix > nowUnix) return true;   // never / clock went back
    return nowUnix - s.lastCheckUnix >= kCheckIntervalSeconds;
}

const char* stageName(Stage s) {
    switch (s) {
        case Stage::Idle: return "idle";
        case Stage::Checking: return "checking";
        case Stage::UpToDate: return "upToDate";
        case Stage::Available: return "available";
        case Stage::Downloading: return "downloading";
        case Stage::Installing: return "installing";
        case Stage::Failed: return "failed";
    }
    return "idle";
}

std::string statusToJson(const Status& s) {
    json::Value o = json::Value::object();
    o.set("stage", stageName(s.stage));
    o.set("current", s.current);
    o.set("latest", s.latest);
    o.set("pageUrl", s.pageUrl);
    o.set("notes", s.notes);
    o.set("canInstall", s.canInstall);
    o.set("progress", s.progress);
    o.set("error", s.error);
    return json::write(o, 0);
}

}  // namespace snow::update
