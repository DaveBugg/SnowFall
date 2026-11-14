#include "update/updater_win.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "diagnostics/log.h"
#include "update/crypto_win.h"

namespace snow::update {

namespace {

constexpr wchar_t kUninstKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SnowFall";   // SnowFallSetup
constexpr DWORD kInstallerWaitMs = 10 * 60 * 1000;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring normalizedDir(std::wstring p) {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n > 0 && n < std::size(buf)) p = buf;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

std::string httpNote(const HttpResult& r) {
    if (r.ok) return {};
    std::string s = " (" + r.reason;
    if (r.error) s += ", error " + std::to_string(r.error);
    return s + ")";
}

}  // namespace

HANDLE launchInstaller(const std::wstring& installerPath) {
    std::wstring cmd = L"\"" + installerPath + L"\" /S /UPDATE";
    std::wstring dir = installerPath;
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(installerPath.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        SNOW_LOGW("update: cannot start the installer (error %lu)", GetLastError());
        return nullptr;
    }
    CloseHandle(pi.hThread);
    SNOW_LOGI("update: installer started (pid %lu) with /S /UPDATE", pi.dwProcessId);
    return pi.hProcess;
}

bool isInstallerCopy(const std::wstring& exePath) {
    wchar_t loc[MAX_PATH * 2] = {};
    DWORD bytes = sizeof loc - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, loc, &bytes) != ERROR_SUCCESS ||
        !loc[0])
        return false;
    const size_t slash = exePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    const std::wstring a = normalizedDir(exePath.substr(0, slash)), b = normalizedDir(loc);
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

void Updater::cleanDownloads(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (DeleteFileW((dir + L"\\" + fd.cFileName).c_str())) ++removed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (removed) SNOW_LOGI("update: removed %d old file(s) from the download folder", removed);
}

struct Updater::State {
    UpdaterConfig cfg;
    std::unique_ptr<HttpTransport> http;
    Launcher launch;
    std::function<void()> notify;

    mutable std::mutex m;
    Status status;
    ReleaseInfo release;      // of the last successful check
    bool busy = false;
    std::atomic<bool> stop{false};
    HANDLE cancelEvent = nullptr;
    std::thread worker;

    void publish(const Status& s) {
        {
            std::lock_guard<std::mutex> lock(m);
            status = s;
        }
        if (notify) notify();
    }
    Status snapshot() const {
        std::lock_guard<std::mutex> lock(m);
        return status;
    }
    void fail(Status s, const char* code) {
        s.stage = Stage::Failed;
        s.error = code;
        s.progress = 0;
        publish(s);
    }

    HttpResult fetch(const std::string& url, Accept accept, std::size_t maxBytes, std::string& out) {
        out.clear();
        return http->get(url, accept, maxBytes, [&](const std::uint8_t* p, std::size_t n, long long) {
            out.append(reinterpret_cast<const char*>(p), n);
            return !stop.load();
        });
    }

    void doCheck() {
        const Version cur = parseVersion(cfg.currentVersion);
        Status s;
        s.current = cur.valid ? cur.text() : cfg.currentVersion;
        s.stage = Stage::Checking;
        publish(s);
        SNOW_LOGI("update: check started (running %s)", cfg.currentVersion.c_str());
        std::string body;
        const HttpResult r = fetch(kLatestReleaseApi, Accept::GitHubJson, kMaxApiBytes, body);
        SNOW_LOGI("update: GET latest release -> HTTP %d, %zu bytes%s", r.status, r.bytes, httpNote(r).c_str());
        if (!r.ok) return fail(s, kErrNetwork);
        ReleaseInfo rel;
        std::string err;
        if (!parseLatestRelease(body, rel, &err)) {
            SNOW_LOGW("update: release JSON rejected (%s)", err.c_str());
            return fail(s, kErrNetwork);
        }
        s.latest = rel.version.text();
        if (!isNewer(rel.version, cur)) {
            s.stage = Stage::UpToDate;
            SNOW_LOGI("update: up to date (latest %s, running %s)", s.latest.c_str(), s.current.c_str());
            publish(s);
            return;
        }
        s.stage = Stage::Available;
        s.pageUrl = rel.pageUrl.empty() ? std::string(kReleasesPage) : rel.pageUrl;
        s.notes = rel.notes;
        const bool assets = installerAsset(rel, Platform::Windows) && findAsset(rel, kChecksumsName) && findAsset(rel, kSignatureName);
        s.canInstall = cfg.installable && assets;
        SNOW_LOGI("update: %s available (running %s; signed installer %s, installed copy %s)", s.latest.c_str(), s.current.c_str(),
                  assets ? "yes" : "no", cfg.installable ? "yes" : "no");
        {
            std::lock_guard<std::mutex> lock(m);
            release = rel;
        }
        publish(s);
    }

    void doInstall() {
        Status s = snapshot();
        ReleaseInfo rel;
        {
            std::lock_guard<std::mutex> lock(m);
            rel = release;
        }
        const Asset* setup = installerAsset(rel, Platform::Windows);
        const Asset* sums = findAsset(rel, kChecksumsName);
        const Asset* sig = findAsset(rel, kSignatureName);
        if (!setup || !sums || !sig) return fail(s, kErrVerify);
        s.stage = Stage::Downloading;
        s.progress = 0;
        s.error.clear();
        publish(s);
        SNOW_LOGI("update: installing %s", s.latest.c_str());

        std::string sumsText, sigBytes;
        HttpResult r = fetch(sums->url, Accept::Binary, kMaxSmallFileBytes, sumsText);
        SNOW_LOGI("update: GET %s -> HTTP %d, %zu bytes%s", kChecksumsName, r.status, r.bytes, httpNote(r).c_str());
        if (!r.ok) return fail(s, kErrNetwork);
        r = fetch(sig->url, Accept::Binary, kMaxSmallFileBytes, sigBytes);
        SNOW_LOGI("update: GET %s -> HTTP %d, %zu bytes%s", kSignatureName, r.status, r.bytes, httpNote(r).c_str());
        if (!r.ok) return fail(s, kErrNetwork);
        const std::vector<std::uint8_t> der(sigBytes.begin(), sigBytes.end());
        const bool signatureOk = verifyReleaseSignature(sumsText, der);
        SNOW_LOGI("update: %s signature %s", kChecksumsName, signatureOk ? "valid" : "INVALID");
        if (!signatureOk) return fail(s, kErrVerify);
        const auto sumsMap = parseChecksums(sumsText);
        const auto expected = sumsMap.find(setup->name);
        if (expected == sumsMap.end()) {
            SNOW_LOGW("update: %s has no entry for %s", kChecksumsName, setup->name.c_str());
            return fail(s, kErrVerify);
        }

        CreateDirectoryW(cfg.downloadDir.c_str(), nullptr);
        const std::wstring path = cfg.downloadDir + L"\\" + widen(setup->name);
        const std::wstring part = path + L".part";
        DeleteFileW(part.c_str());
        DeleteFileW(path.c_str());
        HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            SNOW_LOGW("update: cannot create the download file (error %lu)", GetLastError());
            return fail(s, kErrInstall);
        }
        Sha256 hash;
        bool writeOk = true;
        long long received = 0;
        r = http->get(setup->url, Accept::Binary, kMaxInstallerBytes, [&](const std::uint8_t* p, std::size_t n, long long total) {
            DWORD written = 0;
            if (!WriteFile(f, p, static_cast<DWORD>(n), &written, nullptr) || written != n) {
                writeOk = false;
                return false;
            }
            hash.update(p, n);
            received += static_cast<long long>(n);
            const long long all = total > 0 ? total : setup->size;
            const int pct = all > 0 ? static_cast<int>(std::min<long long>(99, received * 100 / all)) : 0;
            if (pct != s.progress) {
                s.progress = pct;
                publish(s);
            }
            return !stop.load();
        });
        const bool flushed = FlushFileBuffers(f) != FALSE;
        CloseHandle(f);
        SNOW_LOGI("update: GET %s -> HTTP %d, %zu bytes%s", setup->name.c_str(), r.status, r.bytes, httpNote(r).c_str());
        if (!r.ok || !writeOk || !flushed) {
            DeleteFileW(part.c_str());
            return fail(s, writeOk && flushed ? kErrNetwork : kErrInstall);
        }
        std::uint8_t digest[32];
        const bool hashed = hash.finish(digest);
        const std::string actual = hashed ? toHex(digest, 32) : std::string();
        const bool match = hashed && actual == expected->second;
        SNOW_LOGI("update: installer SHA-256 %s", match ? "matches SHA256SUMS" : "MISMATCH");
        if (!match || !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(part.c_str());
            return fail(s, match ? kErrInstall : kErrVerify);
        }
        if (stop.load()) {
            DeleteFileW(path.c_str());
            return;
        }

        s.stage = Stage::Installing;
        s.progress = 100;
        publish(s);
        HANDLE proc = launch ? launch(path) : nullptr;
        if (!proc) {
            DeleteFileW(path.c_str());
            return fail(s, kErrInstall);
        }
        // Normally the installer closes this app (remote "exit") and starts the new version.
        // If it gives up instead, show the failure.
        const HANDLE waits[2] = {proc, cancelEvent};
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, kInstallerWaitMs);
        if (w == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(proc, &code);
            SNOW_LOGI("update: installer exited with code %lu", code);
            if (code != 0) fail(s, kErrInstall);
        }
        CloseHandle(proc);
    }
};

Updater::Updater(UpdaterConfig config, std::unique_ptr<HttpTransport> transport, Launcher launcher, std::function<void()> notify)
    : st_(std::make_shared<State>()) {
    st_->cfg = std::move(config);
    st_->http = std::move(transport);
    st_->launch = std::move(launcher);
    st_->notify = std::move(notify);
    st_->cancelEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const Version cur = parseVersion(st_->cfg.currentVersion);
    st_->status.current = cur.valid ? cur.text() : st_->cfg.currentVersion;
}

Updater::~Updater() {
    shutdown();
    if (st_->cancelEvent) CloseHandle(st_->cancelEvent);
}

bool Updater::busy() const {
    std::lock_guard<std::mutex> lock(st_->m);
    return st_->busy;
}

Status Updater::status() const { return st_->snapshot(); }

bool Updater::check() {
    State& s = *st_;
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.busy || s.stop.load() || !s.http) return false;
        s.busy = true;
    }
    if (s.worker.joinable()) s.worker.join();   // the previous job has finished (busy was false)
    s.worker = std::thread([&s] {
        s.doCheck();
        std::lock_guard<std::mutex> lock(s.m);
        s.busy = false;
    });
    return true;
}

bool Updater::install() {
    State& s = *st_;
    {
        std::lock_guard<std::mutex> lock(s.m);
        if (s.busy || s.stop.load() || !s.http || s.status.stage != Stage::Available || !s.status.canInstall) return false;
        s.busy = true;
    }
    if (s.worker.joinable()) s.worker.join();
    s.worker = std::thread([&s] {
        s.doInstall();
        std::lock_guard<std::mutex> lock(s.m);
        s.busy = false;
    });
    return true;
}

void Updater::shutdown() {
    State& s = *st_;
    s.stop.store(true);
    if (s.cancelEvent) SetEvent(s.cancelEvent);
    if (s.http) s.http->cancel();
    if (s.worker.joinable()) s.worker.join();
}

}  // namespace snow::update
