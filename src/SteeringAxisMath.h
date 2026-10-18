#pragma once

#include <cstdint>

namespace steering_sync {

// DirectInput range is configured as [0, 65535]. The known-working
// SteeringSync DLL normalizes it to [-1, 1], then negates the result
// before sending it to the visual steering_w animation.
// invert=true is a nondefault override, only for physically reversed axes.
constexpr float NormalizeVisualSteering(std::int32_t raw,
                                        bool invert = false) noexcept {
    const float normalized = (static_cast<float>(raw) - 32767.5f) / 32767.5f;
    const float clamped = normalized < -1.0f ? -1.0f
                        : normalized > 1.0f ? 1.0f : normalized;
    return invert ? clamped : -clamped;
}

// Enforced at build time for both ATS and ETS2 DLLs.
static_assert(NormalizeVisualSteering(0) == 1.0f,
              "SteeringSync: visual steering sign wrong at left end");
static_assert(NormalizeVisualSteering(65535) == -1.0f,
              "SteeringSync: visual steering sign wrong at right end");
static_assert(NormalizeVisualSteering(0, true) == -1.0f,
              "SteeringSync: optional axis inversion broken");
static_assert(NormalizeVisualSteering(65535, true) == 1.0f,
              "SteeringSync: optional axis inversion broken");

} // namespace steering_sync
