#pragma once
#include <cstdint>
namespace mgs5vr {
// Exact-build graphics-option adapter. Does not change simulation delta time.
bool enableNativeFrameRate(uintptr_t moduleBase) noexcept;
bool nativeFrameRateEnabled() noexcept;
void paceNativePresent() noexcept;
void recordNativePresent(double captureMs,double pacingMs,double presentMs) noexcept;
void stopNativePerformance() noexcept;
// Publish the XR session's predicted display period (nanoseconds) once the
// session loop is running, so producer pacing can track the real consumer
// cadence instead of assuming a 90 Hz headset. Ignored until OpenXR exists.
void reportConsumerDisplayPeriod(long long periodNs) noexcept;
}
