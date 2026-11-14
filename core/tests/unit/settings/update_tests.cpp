// core/update: versions, release JSON, URL allow-list, checksums, DER → raw signatures.
#include <cstring>

#include "settings/json.h"
#include "snow_test.h"
#include "update/update.h"
#include "update/update_test_vector.h"

using namespace snow;
using namespace snow::update;

TEST(update_version_parse_and_compare) {
    CHECK(parseVersion("0.1.2").valid && parseVersion("0.1.2").text() == "0.1.2");
    CHECK(parseVersion("v1.10.3").text() == "1.10.3");
    CHECK(parseVersion("0.1.2+72061da").text() == "0.1.2");
    CHECK(parseVersion("0.2").text() == "0.2.0");
    CHECK(!parseVersion("").valid && !parseVersion("abc").valid && !parseVersion("1").valid && !parseVersion("1.x").valid);
    CHECK(!parseVersion("1.2.3x").valid);
    CHECK(isNewer(parseVersion("0.1.3"), parseVersion("0.1.2+abc")));
    CHECK(isNewer(parseVersion("0.10.0"), parseVersion("0.9.9")));
    CHECK(!isNewer(parseVersion("0.1.2"), parseVersion("0.1.2")));
    CHECK(!isNewer(parseVersion("0.1.1"), parseVersion("0.1.2")));
    CHECK(!isNewer(parseVersion("junk"), parseVersion("0.1.2")));
}

TEST(update_url_allow_list) {
    CHECK(isAllowedUrl("https://github.com/DaveBugg/SnowFall/releases/download/v0.1.3/SnowFallSetup.exe"));
    CHECK(isAllowedUrl("https://api.github.com/repos/DaveBugg/SnowFall/releases/latest"));
    CHECK(isAllowedUrl("https://objects.githubusercontent.com/github-production-release-asset/1/2?x=y"));
    CHECK(isAllowedUrl("https://release-assets.githubusercontent.com/github-production-release-asset/1"));
    CHECK(!isAllowedUrl("http://github.com/DaveBugg/SnowFall/releases/x"));             // not https
    CHECK(!isAllowedUrl("https://github.com/evil/SnowFall/releases/download/x"));      // other repo
    CHECK(!isAllowedUrl("https://github.com.evil.com/DaveBugg/SnowFall/x"));
    CHECK(!isAllowedUrl("https://user@github.com/DaveBugg/SnowFall/x"));
    CHECK(!isAllowedUrl("https://github.com:8443/DaveBugg/SnowFall/x"));
    CHECK(!isAllowedUrl("https://example.com/SnowFallSetup.exe"));
    CHECK(!isAllowedUrl("https://github.com/DaveBugg/SnowFall/x y"));
}

TEST(update_parse_latest_release) {
    const char* text = R"({
      "tag_name": "v0.1.3", "html_url": "https://github.com/DaveBugg/SnowFall/releases/tag/v0.1.3",
      "draft": false, "prerelease": false, "body": "- Fixes\n- More snow",
      "assets": [
        {"name": "SnowFallSetup.exe", "size": 1459200,
         "browser_download_url": "https://github.com/DaveBugg/SnowFall/releases/download/v0.1.3/SnowFallSetup.exe"},
        {"name": "SnowFall-0.1.3.dmg", "size": 2531027,
         "browser_download_url": "https://github.com/DaveBugg/SnowFall/releases/download/v0.1.3/SnowFall-0.1.3.dmg"},
        {"name": "SHA256SUMS", "size": 300,
         "browser_download_url": "https://github.com/DaveBugg/SnowFall/releases/download/v0.1.3/SHA256SUMS"},
        {"name": "evil.exe", "size": 1, "browser_download_url": "https://evil.example/evil.exe"},
        {"name": "../x.exe", "size": 1,
         "browser_download_url": "https://github.com/DaveBugg/SnowFall/releases/download/v0.1.3/x.exe"}
      ]})";
    ReleaseInfo r;
    std::string err;
    CHECK(parseLatestRelease(text, r, &err));
    CHECK(r.tag == "v0.1.3" && r.version.text() == "0.1.3");
    CHECK(r.pageUrl == "https://github.com/DaveBugg/SnowFall/releases/tag/v0.1.3");
    CHECK(r.notes == "- Fixes\n- More snow");
    CHECK(r.assets.size() == 3);   // the foreign URL and the path name are dropped
    CHECK(installerAsset(r, Platform::Windows) && installerAsset(r, Platform::Windows)->size == 1459200);
    CHECK(installerAsset(r, Platform::MacOS) && installerAsset(r, Platform::MacOS)->name == "SnowFall-0.1.3.dmg");
    CHECK(findAsset(r, kChecksumsName) && !findAsset(r, kSignatureName));

    CHECK(!parseLatestRelease(R"({"tag_name": "v0.2.0", "draft": true})", r));
    CHECK(!parseLatestRelease(R"({"tag_name": "v0.2.0", "prerelease": true})", r));
    CHECK(!parseLatestRelease(R"({"tag_name": "nightly"})", r));
    CHECK(!parseLatestRelease("not json", r));
    ReleaseInfo r2;   // a bad html_url falls back to the releases list
    CHECK(parseLatestRelease(R"({"tag_name": "v0.2.0", "html_url": "https://evil.example/"})", r2));
    CHECK(r2.pageUrl == kReleasesPage && r2.assets.empty());
}

TEST(update_checksums) {
    const auto m = parseChecksums(
        "8E1D8E0EAB8D4E1D8E0EAB8D4E1D8E0EAB8D4E1D8E0EAB8D4E1D8E0EAB8D4E1D  SnowFallSetup.exe\r\n"
        "0000000000000000000000000000000000000000000000000000000000000001 *SnowFall-0.1.3.dmg\n"
        "zz00000000000000000000000000000000000000000000000000000000000001  bad.txt\n"
        "short  x\n"
        "1111111111111111111111111111111111111111111111111111111111111111  ./SHA256SUMS.sig");
    CHECK(m.size() == 3);
    CHECK(m.at("SnowFallSetup.exe") == "8e1d8e0eab8d4e1d8e0eab8d4e1d8e0eab8d4e1d8e0eab8d4e1d8e0eab8d4e1d");
    CHECK(m.count("SnowFall-0.1.3.dmg") == 1 && m.count("SHA256SUMS.sig") == 1);
    const std::uint8_t b[3] = {0x00, 0xab, 0xff};
    CHECK(toHex(b, 3) == "00abff");
}

TEST(update_der_signature_to_raw) {
    std::uint8_t raw[64];
    const std::vector<std::uint8_t> der(kTestSignatureDer, kTestSignatureDer + sizeof kTestSignatureDer);
    CHECK(derSignatureToRaw(der, raw));
    CHECK(toHex(raw, 32) == "e86fbcc47c219591395bea29ae10069dcf80da5df85193bab648d9eb6572be6a");
    CHECK(toHex(raw + 32, 32) == "17dca060c226243e1256525c28acef2ed08d486261405f89e186a440d483b212");
    // Short integers are left-padded; garbage is rejected.
    const std::vector<std::uint8_t> small = {0x30, 0x06, 0x02, 0x01, 0x05, 0x02, 0x01, 0x07};
    CHECK(derSignatureToRaw(small, raw) && raw[31] == 5 && raw[0] == 0 && raw[63] == 7);
    CHECK(!derSignatureToRaw({0x30, 0x06, 0x02, 0x01}, raw));
    std::vector<std::uint8_t> trailing = der;
    trailing.push_back(0);
    CHECK(!derSignatureToRaw(trailing, raw));
    CHECK(kReleasePublicKey[0] == 0x04);
}

TEST(update_auto_check_schedule) {
    UpdateSettings s;
    CHECK(!autoCheckDue(s, 1'800'000'000));   // not agreed
    s.check = true;
    CHECK(autoCheckDue(s, 1'800'000'000));    // never checked
    s.lastCheckUnix = 1'800'000'000;
    CHECK(!autoCheckDue(s, 1'800'000'000 + 3600));
    CHECK(autoCheckDue(s, 1'800'000'000 + kCheckIntervalSeconds));
    CHECK(autoCheckDue(s, 1'700'000'000));    // clock went back: check again
}

TEST(update_status_json) {
    Status st;
    st.stage = Stage::Available;
    st.current = "0.1.2";
    st.latest = "0.1.3";
    st.canInstall = true;
    json::Value v;
    CHECK(json::parse(statusToJson(st), v));
    CHECK(v.find("stage")->asString() == "available" && v.find("latest")->asString() == "0.1.3");
    CHECK(v.find("canInstall")->asBool() && v.find("progress")->asNumber() == 0);
}
