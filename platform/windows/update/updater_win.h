// Updater: check for a new release and install it (Windows). Network and process launch are
// injected (HttpTransport, Launcher), so the logic runs in unit tests without the network.
//
//   check()    one GET of update::kLatestReleaseApi -> UpToDate / Available / Failed("network")
//   install()  SHA256SUMS + SHA256SUMS.sig (ECDSA P-256, release key) -> the installer, whose
//              SHA-256 must match SHA256SUMS -> launch "SnowFallSetup.exe /S /UPDATE"; any
//              mismatch -> Failed("verify") and the file is deleted.
//
// Work runs on one worker thread; every status change calls `notify` from that thread (the app
// posts a message to its UI thread there) and the UI reads status(). Status::error holds a
// reason code: "network", "verify" or "install".
#pragma once
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

#include "update/http_win.h"
#include "update/update.h"

namespace snow::update {

inline constexpr char kErrNetwork[] = "network";
inline constexpr char kErrVerify[] = "verify";
inline constexpr char kErrInstall[] = "install";

// Starts the downloaded installer; returns its process handle (caller closes) or nullptr.
using Launcher = std::function<HANDLE(const std::wstring& installerPath)>;
// Default launcher: CreateProcess "<path>" /S /UPDATE.
HANDLE launchInstaller(const std::wstring& installerPath);

struct UpdaterConfig {
    std::string currentVersion;   // SNOW_VERSION, e.g. "0.1.2+abc1234"
    bool installable = false;     // this copy was installed by SnowFallSetup (see isInstallerCopy)
    std::wstring downloadDir;     // e.g. %LOCALAPPDATA%\SnowFall\updates
};

class Updater {
public:
    Updater(UpdaterConfig config, std::unique_ptr<HttpTransport> transport, Launcher launcher,
            std::function<void()> notify);
    ~Updater();   // shutdown()
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Both return false (and do nothing) while a job runs, or when install() is not possible.
    bool check();
    bool install();
    bool busy() const;
    Status status() const;

    // Aborts the network request in progress and waits for the worker (UI thread, at exit).
    void shutdown();

    // Deletes leftover files in the download folder (start-up). Running installers stay.
    static void cleanDownloads(const std::wstring& dir);

private:
    struct State;
    std::shared_ptr<State> st_;
};

// True when `exePath` lives in the folder SnowFallSetup registered as InstallLocation
// (HKCU\...\Uninstall\SnowFall). Portable / development / MSIX copies return false.
bool isInstallerCopy(const std::wstring& exePath);

}  // namespace snow::update
