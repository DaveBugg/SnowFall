// Windows update module: CNG SHA-256 / ECDSA verifier (real signature made with the release key),
// the updater logic with a fake transport (never the network) and the installer's /UPDATE rules.
#include <windows.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "installer/setup_update.h"
#include "snow_test.h"
#include "update/crypto_win.h"
#include "update/http_win.h"
#include "update/update.h"
#include "update/update_test_vector.h"
#include "update/updater_win.h"

using namespace snow::update;

namespace {

std::string sha256Hex(const std::string& s) {
    std::uint8_t d[32];
    if (!Sha256::digest(s.data(), s.size(), d)) return {};
    return toHex(d, 32);
}

std::vector<std::uint8_t> testSignature() { return {kTestSignatureDer, kTestSignatureDer + sizeof kTestSignatureDer}; }
std::string testText() { return std::string(kTestSignedText, sizeof kTestSignedText - 1); }

// Serves canned responses; records every URL asked for. Never touches the network.
class FakeTransport final : public HttpTransport {
public:
    struct Reply {
        int status = 200;
        std::string body;
    };
    std::map<std::string, Reply> replies;
    std::vector<std::string>* requested = nullptr;

    HttpResult get(const std::string& url, Accept, std::size_t maxBytes, const HttpSink& sink) override {
        if (requested) requested->push_back(url);
        HttpResult r;
        const auto it = replies.find(url);
        if (!isAllowedUrl(url) || it == replies.end()) {
            r.reason = "no reply";
            return r;
        }
        r.status = it->second.status;
        if (r.status != 200) return r;
        if (it->second.body.size() > maxBytes) return r;
        if (!it->second.body.empty() &&
            !sink(reinterpret_cast<const std::uint8_t*>(it->second.body.data()), it->second.body.size(),
                  static_cast<long long>(it->second.body.size())))
            return r;
        r.bytes = it->second.body.size();
        r.ok = true;
        return r;
    }
    void cancel() override {}
};

const std::string kBase = "https://github.com/DaveBugg/SnowFall/releases/download/v99.0.0/";

std::string releaseJson(const std::string& tag, bool signedAssets) {
    std::string assets = R"({"name": "SnowFallSetup.exe", "size": 3, "browser_download_url": ")" + kBase + R"(SnowFallSetup.exe"})";
    if (signedAssets) {
        assets += R"(, {"name": "SHA256SUMS", "size": 10, "browser_download_url": ")" + kBase + R"(SHA256SUMS"})";
        assets += R"(, {"name": "SHA256SUMS.sig", "size": 71, "browser_download_url": ")" + kBase + R"(SHA256SUMS.sig"})";
    }
    return R"({"tag_name": ")" + tag + R"(", "html_url": "https://github.com/DaveBugg/SnowFall/releases/tag/)" + tag +
           R"(", "draft": false, "prerelease": false, "body": "notes", "assets": [)" + assets + "]}";
}

std::wstring tempDir() {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"snow_update_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

int filesIn(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

struct Harness {
    std::vector<std::string> requested;
    int launches = 0;
    std::unique_ptr<Updater> updater;
    std::wstring dir = tempDir();

    explicit Harness(std::map<std::string, FakeTransport::Reply> replies, bool installable = true) {
        auto t = std::make_unique<FakeTransport>();
        t->replies = std::move(replies);
        t->requested = &requested;
        UpdaterConfig cfg;
        cfg.currentVersion = "0.1.2+abc1234";
        cfg.installable = installable;
        cfg.downloadDir = dir;
        updater = std::make_unique<Updater>(std::move(cfg), std::move(t),
                                            [this](const std::wstring&) -> HANDLE {
                                                ++launches;
                                                return nullptr;
                                            },
                                            nullptr);
    }
    ~Harness() {
        updater.reset();
        Updater::cleanDownloads(dir);
        RemoveDirectoryW(dir.c_str());
    }
    Status wait() {
        for (int i = 0; i < 500 && updater->busy(); ++i) Sleep(10);
        return updater->status();
    }
};

}  // namespace

TEST(update_win_sha256_known_vectors) {
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Sha256 h;   // incremental == one shot
    CHECK(h.ok());
    h.update("a", 1);
    h.update("bc", 2);
    std::uint8_t d[32];
    CHECK(h.finish(d) && toHex(d, 32) == sha256Hex("abc"));
}

TEST(update_win_signature_test_vector) {
    const std::string text = testText();
    const std::vector<std::uint8_t> sig = testSignature();
    CHECK(verifyReleaseSignature(text, sig));

    std::string badText = text;   // one flipped byte in the signed text
    badText[0] ^= 0x01;
    CHECK(!verifyReleaseSignature(badText, sig));
    CHECK(!verifyReleaseSignature(text + "x", sig));

    std::vector<std::uint8_t> badSig = sig;   // one flipped byte in r, then in s
    badSig[10] ^= 0x01;
    CHECK(!verifyReleaseSignature(text, badSig));
    badSig = sig;
    badSig[sig.size() - 3] ^= 0x80;
    CHECK(!verifyReleaseSignature(text, badSig));
    CHECK(!verifyReleaseSignature(text, {}));

    std::uint8_t otherKey[65];   // another public key (one coordinate byte changed): rejected
    std::memcpy(otherKey, kReleasePublicKey, 65);
    otherKey[64] ^= 0x01;
    CHECK(!verifySignature(text, sig, otherKey));
}

TEST(update_win_check_available_and_up_to_date) {
    {
        Harness h({{kLatestReleaseApi, {200, releaseJson("v99.0.0", true)}}});
        CHECK(h.updater->status().stage == Stage::Idle && h.updater->status().current == "0.1.2");
        CHECK(h.updater->check());
        const Status st = h.wait();
        CHECK(st.stage == Stage::Available && st.latest == "99.0.0" && st.canInstall);
        CHECK(st.pageUrl == "https://github.com/DaveBugg/SnowFall/releases/tag/v99.0.0" && st.notes == "notes");
        CHECK(h.requested.size() == 1 && h.requested[0] == kLatestReleaseApi);   // exactly one request
    }
    {
        Harness h({{kLatestReleaseApi, {200, releaseJson("v99.0.0", true)}}}, /*installable*/ false);
        h.updater->check();
        const Status st = h.wait();
        CHECK(st.stage == Stage::Available && !st.canInstall);   // portable copy: manual install
        CHECK(!h.updater->install());
    }
    {
        Harness h({{kLatestReleaseApi, {200, releaseJson("v99.0.0", false)}}});
        h.updater->check();
        CHECK(h.wait().stage == Stage::Available && !h.updater->status().canInstall);   // unsigned release
    }
    {
        Harness h({{kLatestReleaseApi, {200, releaseJson("v0.1.2", true)}}});
        h.updater->check();
        const Status st = h.wait();
        CHECK(st.stage == Stage::UpToDate && st.latest == "0.1.2");
        CHECK(!h.updater->install());
    }
    {
        Harness h({{kLatestReleaseApi, {503, ""}}});
        h.updater->check();
        const Status st = h.wait();
        CHECK(st.stage == Stage::Failed && st.error == kErrNetwork);
    }
    {
        Harness h({{kLatestReleaseApi, {200, "not json"}}});
        h.updater->check();
        CHECK(h.wait().stage == Stage::Failed);
    }
}

TEST(update_win_install_rejects_bad_signature_and_checksums) {
    const std::string sigBytes(reinterpret_cast<const char*>(kTestSignatureDer), sizeof kTestSignatureDer);
    {   // tampered SHA256SUMS: the signature check fails, nothing is downloaded or started
        std::string tampered = testText();
        tampered[0] = 'b';
        Harness h({{kLatestReleaseApi, {200, releaseJson("v99.0.0", true)}},
                   {kBase + "SHA256SUMS", {200, tampered}},
                   {kBase + "SHA256SUMS.sig", {200, sigBytes}},
                   {kBase + "SnowFallSetup.exe", {200, "abc"}}});
        h.updater->check();
        h.wait();
        CHECK(h.updater->install());
        const Status st = h.wait();
        CHECK(st.stage == Stage::Failed && st.error == kErrVerify && st.latest == "99.0.0");
        CHECK(h.launches == 0 && filesIn(h.dir) == 0);
        for (const std::string& u : h.requested) CHECK(u != kBase + "SnowFallSetup.exe");
    }
    {   // valid signature, but SHA256SUMS has no valid entry for the installer
        Harness h({{kLatestReleaseApi, {200, releaseJson("v99.0.0", true)}},
                   {kBase + "SHA256SUMS", {200, testText()}},
                   {kBase + "SHA256SUMS.sig", {200, sigBytes}},
                   {kBase + "SnowFallSetup.exe", {200, "abc"}}});
        h.updater->check();
        h.wait();
        CHECK(h.updater->install());
        const Status st = h.wait();
        CHECK(st.stage == Stage::Failed && st.error == kErrVerify);
        CHECK(h.launches == 0 && filesIn(h.dir) == 0);
    }
    {   // the signature file is missing on the server
        Harness h({{kLatestReleaseApi, {200, releaseJson("v99.0.0", true)}}, {kBase + "SHA256SUMS", {200, testText()}}});
        h.updater->check();
        h.wait();
        CHECK(h.updater->install());
        const Status st = h.wait();
        CHECK(st.stage == Stage::Failed && st.error == kErrNetwork && h.launches == 0);
    }
}

TEST(update_win_installer_copy_detection) {
    // A path outside any registered install location is never "installed by the installer".
    CHECK(!isInstallerCopy(L"C:\\definitely\\not\\installed\\here\\SnowFall.exe"));
    CHECK(!isInstallerCopy(L""));
}

TEST(update_win_installer_update_switch) {
    using namespace snow::setup;
    CHECK(isUpdateSwitch(L"/UPDATE") && isUpdateSwitch(L"/update") && !isUpdateSwitch(L"/S") && !isUpdateSwitch(nullptr));
    ExistingInstall e;   // nothing present: an update must not add shortcuts or autostart
    InstallChoices c = keepExistingChoices(e);
    CHECK(!c.startMenu && !c.desktop && !c.autostart);
    e.startMenuShortcut = true;
    e.desktopShortcut = true;
    c = keepExistingChoices(e);
    CHECK(c.startMenu && c.desktop && !c.autostart);   // autostart stays off (a plain /S would turn it on)
    e = ExistingInstall{};
    e.autostart = true;
    c = keepExistingChoices(e);
    CHECK(!c.startMenu && !c.desktop && c.autostart);
    const InstallChoices defaults;   // the plain silent install defaults are unchanged
    CHECK(defaults.startMenu && !defaults.desktop && defaults.autostart);
}
