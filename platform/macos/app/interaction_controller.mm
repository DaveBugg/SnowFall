#include "app/interaction_controller.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <cmath>

#include "diagnostics/log.h"
#include "runtime/sim_runner.h"
#include "system/platform_services.h"

namespace snow::mac {
namespace {

constexpr double kPollSeconds = 0.020;
// The chord must be held this long (ignores quick shortcuts such as Ctrl+Option+S). Short,
// so that "hold keys, then press the mouse" is already captured by the tap.
constexpr double kHoldDelay = 0.060;
constexpr double kDeliberateHold = 0.6;   // a hold this long asks for the permission once

double nowSeconds() { return CFAbsoluteTimeGetCurrent(); }

bool chordInFlags(NSEventModifierFlags f) {
    const bool ctrl = (f & NSEventModifierFlagControl) != 0, opt = (f & NSEventModifierFlagOption) != 0;
    const bool shift = (f & NSEventModifierFlagShift) != 0, cmd = (f & NSEventModifierFlagCommand) != 0;
    return ctrl && opt && !shift && !cmd;
}

bool chordInEventFlags(CGEventFlags f) {
    const bool ctrl = (f & kCGEventFlagMaskControl) != 0, opt = (f & kCGEventFlagMaskAlternate) != 0;
    const bool shift = (f & kCGEventFlagMaskShift) != 0, cmd = (f & kCGEventFlagMaskCommand) != 0;
    return ctrl && opt && !shift && !cmd;
}

bool pointerNow(float& x, float& y) {
    CGEventRef e = CGEventCreate(nullptr);
    if (!e) return false;
    const CGPoint p = CGEventGetLocation(e);   // global, top-left origin, points
    CFRelease(e);
    x = static_cast<float>(p.x);
    y = static_cast<float>(p.y);
    return true;
}

}  // namespace

struct InteractionController::Impl {
    InteractionController* owner = nullptr;
    NSTimer* timer = nil;
    CFMachPortRef tap = nullptr;
    CFRunLoopSourceRef source = nullptr;
    double lastTrustCheck = 0;

    bool hit(float x, float y) const {
        const auto snap = owner->snapshots_.latest();
        if (!snap) return false;
        const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
        for (const RectI& r : snap->hitRegions)
            if (r.contains(ix, iy)) return true;
        return false;
    }

    void post(InputEvent::Type type, float x, float y) {
        InputEvent ev;
        ev.type = type;
        ev.x = x;
        ev.y = y;
        owner->runner_.postInput(ev);
    }

    void install() {
        if (tap) return;
        const CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDown) |
                                 CGEventMaskBit(kCGEventLeftMouseUp) | CGEventMaskBit(kCGEventLeftMouseDragged) |
                                 CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
                                 CGEventMaskBit(kCGEventRightMouseDragged);
        tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, &Impl::callback,
                               this);
        if (!tap) {
            SNOW_LOGI("interaction: event tap unavailable (Accessibility permission not granted) - view only");
            return;
        }
        source = CFMachPortCreateRunLoopSource(nullptr, tap, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
        CGEventTapEnable(tap, true);
        SNOW_LOGI("interaction: event tap installed");
    }

    void remove() {
        if (!tap) return;
        CGEventTapEnable(tap, false);
        if (source) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
            CFRelease(source);
            source = nullptr;
        }
        CFMachPortInvalidate(tap);
        CFRelease(tap);
        tap = nullptr;
    }

    static CGEventRef callback(CGEventTapProxy, CGEventType type, CGEventRef ev, void* user) {
        auto* d = static_cast<Impl*>(user);
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            if (d->tap) CGEventTapEnable(d->tap, true);
            return ev;
        }
        InteractionController& c = *d->owner;
        if (!c.active_) return ev;
        const CGPoint p = CGEventGetLocation(ev);
        const float x = static_cast<float>(p.x), y = static_cast<float>(p.y);
        // `held_` lags the keys by kHoldDelay; also accept the chord being down right now so a
        // quick "keys + click" never leaks to the app below.
        const bool captureAll = c.blockWhileHeld_ && (c.held_ || (c.holdEnabled_ && chordInEventFlags(CGEventGetFlags(ev))));
        switch (type) {
            case kCGEventMouseMoved:
                c.runner_.setPointer(x, y);
                return ev;
            case kCGEventLeftMouseDragged:
                c.runner_.setPointer(x, y);
                return c.capturing_ ? nullptr : ev;
            case kCGEventLeftMouseDown:
                if (!c.capturing_ && (captureAll || d->hit(x, y))) {
                    c.capturing_ = true;
                    c.runner_.setPointer(x, y);
                    d->post(InputEvent::Type::PointerDown, x, y);
                    return nullptr;
                }
                return ev;
            case kCGEventLeftMouseUp:
                if (c.capturing_) {
                    c.capturing_ = false;
                    d->post(InputEvent::Type::PointerUp, x, y);
                    return nullptr;
                }
                return ev;
            case kCGEventRightMouseDown:
                if (!c.capturing_ && (captureAll || d->hit(x, y))) {
                    c.swallowRightUp_ = true;
                    InteractionController* owner = &c;
                    dispatch_async(dispatch_get_main_queue(), ^{
                      if (owner->onCycleTool) owner->onCycleTool();
                    });
                    return nullptr;
                }
                return ev;
            case kCGEventRightMouseDragged:
                return c.swallowRightUp_ ? nullptr : ev;
            case kCGEventRightMouseUp:
                if (c.swallowRightUp_) {
                    c.swallowRightUp_ = false;
                    return nullptr;
                }
                return ev;
            default:
                return ev;
        }
    }
};

InteractionController::InteractionController(SimRunner& runner, const SnapshotExchange& snapshots)
    : runner_(runner), snapshots_(snapshots), impl_(std::make_unique<Impl>()) {
    impl_->owner = this;
}

InteractionController::~InteractionController() { shutdown(); }

std::string InteractionController::chordName() { return "Ctrl+Option"; }

void InteractionController::start() {
    if (impl_->timer) return;
    InteractionController* self = this;
    impl_->timer = [NSTimer timerWithTimeInterval:kPollSeconds
                                          repeats:YES
                                            block:^(NSTimer*) {
                                              self->poll();
                                            }];
    [NSRunLoop.mainRunLoop addTimer:impl_->timer forMode:NSRunLoopCommonModes];
    SNOW_LOGI("interaction: hold chord = %s", chordName().c_str());
}

void InteractionController::shutdown() {
    if (impl_->timer) {
        [impl_->timer invalidate];
        impl_->timer = nil;
    }
    impl_->remove();
}

bool InteractionController::tapInstalled() const { return impl_->tap != nullptr; }

void InteractionController::setFeatureEnabled(bool enabled) {
    feature_ = enabled;
    if (!enabled) toggled_ = false;
    evaluate();
}

void InteractionController::setHoldKeyEnabled(bool enabled) {
    holdEnabled_ = enabled;
    if (!enabled) held_ = false;
    evaluate();
}

void InteractionController::setSuspended(bool suspended) {
    suspended_ = suspended;
    evaluate();
}

void InteractionController::setToggled(bool on) {
    toggled_ = on && feature_;
    evaluate();
    if (toggled_ && active_ && !impl_->tap && !askedForPermission_ && NSApp.modalWindow == nil) {
        askedForPermission_ = true;
        if (onNeedsAccessibility) onNeedsAccessibility();
    }
}

void InteractionController::poll() {
    const double now = nowSeconds();
    bool chord = false;
    if (holdEnabled_ && feature_ && !suspended_) chord = chordInFlags(NSEvent.modifierFlags);
    if (!chord) {
        chordSince_ = 0;
        // Keep the mode alive while a captured drag is still in progress.
        if (held_ && !capturing_) {
            held_ = false;
            evaluate();
        }
    } else {
        if (chordSince_ == 0) chordSince_ = now;
        if (!held_ && now - chordSince_ >= kHoldDelay) {
            held_ = true;
            evaluate();
        }
        // Not while another dialog (the first-run welcome) is open: it would end up behind it.
        // The next poll after the dialog closes asks if the chord is still held.
        if (held_ && active_ && !impl_->tap && !askedForPermission_ && NSApp.modalWindow == nil &&
            now - chordSince_ >= kDeliberateHold) {
            askedForPermission_ = true;
            if (onNeedsAccessibility) onNeedsAccessibility();
        }
    }
    if (toggled_ && active_ && !impl_->tap && !askedForPermission_ && NSApp.modalWindow == nil) {
        askedForPermission_ = true;   // toggled on while a dialog was open: ask now
        if (onNeedsAccessibility) onNeedsAccessibility();
    }
    if (active_ && !impl_->tap) {
        // View-only mode: follow the pointer by polling; pick up a permission granted meanwhile.
        float x, y;
        if (pointerNow(x, y)) runner_.setPointer(x, y);
        if (now - impl_->lastTrustCheck > 1.0) {
            impl_->lastTrustCheck = now;
            if (accessibilityTrusted()) impl_->install();
        }
    }
}

void InteractionController::evaluate() {
    const bool want = feature_ && !suspended_ && (toggled_ || held_);
    if (want == active_) return;
    active_ = want;
    SNOW_LOGI("interaction mode %s", active_ ? "on" : "off");
    if (active_) {
        impl_->install();
        float x, y;
        if (pointerNow(x, y)) runner_.setPointer(x, y);
    } else {
        if (capturing_) {
            float x = 0, y = 0;
            pointerNow(x, y);
            impl_->post(InputEvent::Type::PointerUp, x, y);
            capturing_ = false;
        }
        swallowRightUp_ = false;
        impl_->remove();
    }
    runner_.setInteractionMode(active_);
    if (onActiveChanged) onActiveChanged(active_);
}

}  // namespace snow::mac
