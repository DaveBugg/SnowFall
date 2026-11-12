// Windows services for the portable simulation thread (core/runtime/sim_runner.h):
// high-resolution waitable-timer waits, cursor position and the thread name.
#pragma once
#include "runtime/sim_runner.h"

namespace snow {

SimPlatformHooks makeWindowsSimHooks();

}  // namespace snow
