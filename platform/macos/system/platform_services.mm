#include "system/platform_services.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <ServiceManagement/ServiceManagement.h>
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "diagnostics/log.h"
#include "system/cursor_hider.h"
#include "system/hotkey_map.h"

namespace snow::mac {

// =========================================================================== autostart
namespace autostart {

Status status() {
    switch (SMAppService.mainAppService.status) {
        case SMAppServiceStatusEnabled: return Status::Enabled;
        case SMAppServiceStatusRequiresApproval: return Status::RequiresApproval;
        case SMAppServiceStatusNotRegistered: return Status::Disabled;
        case SMAppServiceStatusNotFound: return Status::Disabled;
    }
    return Status::Unavailable;
}

bool setEnabled(bool on, std::string* error) {
    SMAppService* svc = SMAppService.mainAppService;
    NSError* err = nil;
    const BOOL ok = on ? [svc registerAndReturnError:&err] : [svc unregisterAndReturnError:&err];
    if (!ok) {
        // Unregistering something that is not registered is not an error for us.
        if (!on && svc.status == SMAppServiceStatusNotRegistered) return true;
        SNOW_LOGW("autostart: %s failed (code %ld)", on ? "register" : "unregister", static_cast<long>(err.code));
        if (error) *error = err.localizedDescription.UTF8String ?: "failed";
        return false;
    }
    SNOW_LOGI("autostart: %s, status %ld", on ? "registered" : "unregistered", static_cast<long>(svc.status));
    return true;
}

void openLoginItemsSettings() { [SMAppService openSystemSettingsLoginItems]; }

}  // namespace autostart

// =========================================================================== hotkeys
namespace {
constexpr OSType kSignature = 'SnwF';
}

struct HotkeyManager::Impl {
    HotkeyManager* owner = nullptr;
    EventHandlerRef handler = nullptr;
    std::map<int, EventHotKeyRef> refs;

    static OSStatus onEvent(EventHandlerCallRef, EventRef ev, void* user) {
        EventHotKeyID hid{};
        if (GetEventParameter(ev, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof hid, nullptr, &hid) != noErr)
            return eventNotHandledErr;
        if (hid.signature != kSignature) return eventNotHandledErr;
        auto* self = static_cast<Impl*>(user);
        const int id = static_cast<int>(hid.id);
        // Leave the Carbon callback before running app code (it may re-register hotkeys).
        HotkeyManager* owner = self->owner;
        dispatch_async(dispatch_get_main_queue(), ^{
          if (owner->onHotkey) owner->onHotkey(id);
        });
        return noErr;
    }
};

HotkeyManager::HotkeyManager() : impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
    EventTypeSpec spec{kEventClassKeyboard, kEventHotKeyPressed};
    InstallEventHandler(GetApplicationEventTarget(), &Impl::onEvent, 1, &spec, impl_.get(), &impl_->handler);
}

HotkeyManager::~HotkeyManager() {
    unregisterAll();
    if (impl_->handler) RemoveEventHandler(impl_->handler);
}

bool HotkeyManager::registerHotkey(int id, const Hotkey& hk) {
    unregister(id);
    unsigned key = 0;
    if (!hk.valid() || !vkToMacKeyCode(hk.vk, key)) return false;
    EventHotKeyID hid{kSignature, static_cast<UInt32>(id)};
    EventHotKeyRef ref = nullptr;
    const OSStatus st =
        RegisterEventHotKey(key, modifiersToCarbon(hk.modifiers), hid, GetApplicationEventTarget(), 0, &ref);
    if (st != noErr || !ref) {
        SNOW_LOGW("hotkeys: RegisterEventHotKey failed (%d)", static_cast<int>(st));
        return false;
    }
    impl_->refs[id] = ref;
    return true;
}

void HotkeyManager::unregister(int id) {
    auto it = impl_->refs.find(id);
    if (it == impl_->refs.end()) return;
    UnregisterEventHotKey(it->second);
    impl_->refs.erase(it);
}

void HotkeyManager::unregisterAll() {
    for (auto& [id, ref] : impl_->refs) UnregisterEventHotKey(ref);
    impl_->refs.clear();
}

bool HotkeyManager::reservedBySystem(const Hotkey& hk) {
    unsigned key = 0;
    if (!vkToMacKeyCode(hk.vk, key)) return false;
    const unsigned mods = modifiersToCarbon(hk.modifiers);
    CFArrayRef list = nullptr;
    if (CopySymbolicHotKeys(&list) != noErr || !list) return false;
    bool reserved = false;
    for (CFIndex i = 0; i < CFArrayGetCount(list) && !reserved; ++i) {
        auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        auto enabled = static_cast<CFBooleanRef>(CFDictionaryGetValue(dict, kHISymbolicHotKeyEnabled));
        if (enabled && !CFBooleanGetValue(enabled)) continue;
        int code = -1, m = 0;
        if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, kHISymbolicHotKeyCode))) CFNumberGetValue(n, kCFNumberIntType, &code);
        if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, kHISymbolicHotKeyModifiers))) CFNumberGetValue(n, kCFNumberIntType, &m);
        const unsigned relevant = kCarbonCmd | kCarbonShift | kCarbonOption | kCarbonControl;
        if (code == static_cast<int>(key) && (static_cast<unsigned>(m) & relevant) == mods) reserved = true;
    }
    CFRelease(list);
    return reserved;
}

bool HotkeyManager::probe(const Hotkey& hk) {
    unsigned key = 0;
    if (!hk.valid() || !vkToMacKeyCode(hk.vk, key)) return false;
    EventHotKeyID hid{kSignature, 0x7FFF};
    EventHotKeyRef ref = nullptr;
    // Exclusive: fails when another process registered the same combination exclusively.
    const OSStatus st = RegisterEventHotKey(key, modifiersToCarbon(hk.modifiers), hid, GetApplicationEventTarget(),
                                            kEventHotKeyExclusive, &ref);
    if (st != noErr || !ref) return false;
    UnregisterEventHotKey(ref);
    return true;
}

// =========================================================================== crashes
namespace {
char g_crashPath[1024] = {};

void writeAll(int fd, const char* s) {
    std::size_t n = std::strlen(s);
    while (n > 0) {
        const ssize_t w = ::write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= static_cast<std::size_t>(w);
    }
}

void signalHandler(int sig) {
    CursorHider::emergencyShow();   // never leave the user without a cursor
    // Async-signal context: only open/write/backtrace, no allocation, no locks.
    const int fd = ::open(g_crashPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char num[16];
        int v = sig, i = 15;
        num[i] = '\0';
        do {
            num[--i] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v > 0 && i > 0);
        writeAll(fd, "SnowFall " SNOW_VERSION " crashed: signal ");
        writeAll(fd, num + i);
        writeAll(fd, "\nBacktrace:\n");
        void* frames[64];
        const int n = backtrace(frames, 64);
        backtrace_symbols_fd(frames, n, fd);
        ::close(fd);
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// SIGTERM / SIGINT / SIGHUP: give the cursor back, then die the default way.
void terminationHandler(int sig) {
    CursorHider::emergencyShow();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

void exceptionHandler(NSException* e) {
    CursorHider::emergencyShow();
    FILE* f = std::fopen(g_crashPath, "a");
    if (f) {
        NSString* reason = e.reason ?: @"";
        if (reason.length > 300) reason = [reason substringToIndex:300];
        std::fprintf(f, "SnowFall %s: uncaught exception %s: %s\nBacktrace:\n", SNOW_VERSION, e.name.UTF8String ?: "?",
                     reason.UTF8String ?: "");
        for (NSString* line in e.callStackSymbols) std::fprintf(f, "%s\n", line.UTF8String ?: "");
        std::fclose(f);
    }
    SNOW_LOGE("uncaught exception %s", e.name.UTF8String ?: "?");
}
}  // namespace

void installCrashHandler(const std::string& dir) {
    // Keep only the 5 newest reports (names sort by time): the folder never piles up.
    if (DIR* d = ::opendir(dir.c_str())) {
        std::vector<std::string> names;
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("crash-", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0) names.push_back(n);
        }
        ::closedir(d);
        std::sort(names.begin(), names.end());
        for (size_t i = 0; i + 5 < names.size(); ++i) ::unlink((dir + "/" + names[i]).c_str());
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    std::snprintf(g_crashPath, sizeof g_crashPath, "%s/crash-%04d%02d%02d-%02d%02d%02d.txt", dir.c_str(),
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    NSSetUncaughtExceptionHandler(&exceptionHandler);
    // Alternate stack so a stack overflow can still be reported.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_handler = &signalHandler;
    sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) sigaction(sig, &sa, nullptr);
    struct sigaction term{};
    term.sa_handler = &terminationHandler;
    term.sa_flags = SA_RESETHAND;
    sigemptyset(&term.sa_mask);
    for (int sig : {SIGTERM, SIGINT, SIGHUP}) sigaction(sig, &term, nullptr);
}

// =========================================================================== accessibility
bool accessibilityTrusted() { return AXIsProcessTrusted(); }

void requestAccessibility() {
    NSDictionary* opts = @{(__bridge id)kAXTrustedCheckOptionPrompt : @YES};
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
}

void openAccessibilitySettings() {
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
    [NSWorkspace.sharedWorkspace openURL:url];
}

}  // namespace snow::mac
