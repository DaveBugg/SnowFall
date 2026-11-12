#include "app/sim_platform_win.h"

#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace snow {
namespace {

// Auto-reset event for commands + a high-resolution waitable timer for the next step
// (std::condition_variable timeouts on MSVC are only ~15 ms accurate).
class WinWaiter final : public SimWaiter {
public:
    WinWaiter() {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~WinWaiter() override {
        if (wake_) CloseHandle(wake_);
        if (timer_) CloseHandle(timer_);
    }
    void wake() override { SetEvent(wake_); }
    void wait(double timeoutSeconds) override {
        if (timeoutSeconds < 0) {
            WaitForSingleObject(wake_, INFINITE);
            return;
        }
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(timeoutSeconds * 1e7);
        if (due.QuadPart > -1000) due.QuadPart = -1000;
        SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0);
        HANDLE handles[2] = {timer_, wake_};
        WaitForMultipleObjects(2, handles, FALSE, 100);
    }

private:
    HANDLE wake_ = nullptr;
    HANDLE timer_ = nullptr;
};

}  // namespace

SimPlatformHooks makeWindowsSimHooks() {
    SimPlatformHooks h;
    h.onThreadStart = [] { SetThreadDescription(GetCurrentThread(), L"SnowSimulation"); };
    h.cursor = [](float& x, float& y) {
        POINT pt;
        if (!GetCursorPos(&pt)) return false;   // fails on the secure desktop
        x = static_cast<float>(pt.x);
        y = static_cast<float>(pt.y);
        return true;
    };
    h.makeWaiter = [] { return std::unique_ptr<SimWaiter>(new WinWaiter()); };
    return h;
}

}  // namespace snow
