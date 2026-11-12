// PerformanceGovernor: pure decision logic turning user settings + system state into
// what the app should do right now (simulate? show overlays? quality?). No Win32 here.
#pragma once
#include "app/sim_runner.h"
#include "core/settings.h"

namespace snow {

struct GovernorInputs {
    bool userPaused = false;        // settings.enabled == false
    bool hideAll = false;           // tray «Скрыть всё»
    bool fullscreenApp = false;     // from DesktopLayout
    bool sessionLocked = false;     // WTS lock / disconnect
    bool suspended = false;         // between PBT_APMSUSPEND and resume
    bool onBattery = false;
    bool batterySaver = false;
    bool renderFailed = false;      // unrecoverable device loss
    bool interactionMode = false;
};

enum class PauseReason { None, User, Hidden, Fullscreen, Locked, Suspended, RenderFailed };

struct GovernorDecision {
    SimMode simMode = SimMode::Running;
    bool overlaysVisible = true;
    int fpsCap = 60;
    RuntimeTuning tuning;
    PauseReason reason = PauseReason::None;
    bool autoPaused = false;        // paused by the system state rather than by the user
};

inline GovernorDecision decide(const Settings& s, const GovernorInputs& in) {
    GovernorDecision d;

    // Quality.
    d.fpsCap = (s.performance.fpsCap <= 45 || s.performance.profile == PerfProfile::Low) ? 30 : 60;
    d.tuning.animationsEnabled = s.performance.animations;
    d.tuning.particleScale = 1.0f;
    if (s.performance.reduceOnBattery) {
        if (in.batterySaver) {
            d.tuning.particleScale = 0.35f;
            d.fpsCap = 30;
        } else if (in.onBattery) {
            d.tuning.particleScale = 0.6f;
            d.fpsCap = 30;
        }
    }

    // Visibility / pausing, strongest reason first.
    if (in.renderFailed) {
        d.reason = PauseReason::RenderFailed;
    } else if (in.suspended) {
        d.reason = PauseReason::Suspended;
    } else if (in.sessionLocked) {
        d.reason = PauseReason::Locked;
    } else if (in.hideAll) {
        d.reason = PauseReason::Hidden;
    } else if (s.performance.pauseOnFullscreen && in.fullscreenApp) {
        d.reason = PauseReason::Fullscreen;
    } else if (in.userPaused) {
        d.reason = PauseReason::User;
    }

    switch (d.reason) {
        case PauseReason::None:
            break;
        case PauseReason::User:
            d.simMode = SimMode::Paused;     // drifts stay visible, emission stops
            break;
        default:
            d.simMode = SimMode::Frozen;
            d.overlaysVisible = false;
            d.autoPaused = d.reason != PauseReason::Hidden;
            break;
    }
    return d;
}

}  // namespace snow
