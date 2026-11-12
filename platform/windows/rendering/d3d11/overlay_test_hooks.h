// Internal test hooks of the renderer (used by tests/integration/render). Not part of the
// OverlaySystem contract; the application must not call these.
#pragma once

namespace snow::render_test {

// Makes the render thread treat its next frame as "device removed" and run the normal
// recovery path. `failRecreateAttempts` device re-creations are then made to fail
// artificially (3 or more exercises the give-up path: stats().failed + failedMsg).
// The environment variable SNOW_TEST_DEVICE_LOSS=N does the same once after N presented frames.
void forceDeviceLoss(int failRecreateAttempts = 0);

// Reference count the ID3D11Device reported when the render thread released it for the last
// time (0 = no live device children were left behind). ~0ul until a device has been released.
unsigned long lastDeviceRefCount();

}  // namespace snow::render_test
