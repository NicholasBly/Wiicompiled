#pragma once

#include <array>
#include <cstdint>
#include <optional>

// Saved frame pacing: the mode for inserted presents (Aurora's AURORA_FRAME_PACING_* values) and
// the frame-rate cap they aim for.
namespace FramePacingSetting {

inline constexpr uint32_t kOff = 0;
inline constexpr uint32_t kSmooth = 1;
inline constexpr uint32_t kResponsive = 2;
// 0 matches the display.
inline constexpr std::array<uint32_t, 8> kCaps{0, 120, 144, 165, 180, 200, 240, 360};

struct Setting {
    uint32_t mode = kOff;
    uint32_t cap = 0;
};

inline bool IsSupportedMode(uint32_t mode) { return mode <= kResponsive; }

inline bool IsSupportedCap(uint32_t cap) {
    for (const uint32_t supported : kCaps) {
        if (supported == cap) {
            return true;
        }
    }
    return false;
}

// The saved video.frame_pacing_mode / frame_rate_cap win. Without them, the older
// video.frame_interpolation_fps (120/180, or 240 from an early build) becomes smooth pacing capped
// at that rate.
inline Setting Resolve(std::optional<uint32_t> mode, std::optional<uint32_t> cap,
                       std::optional<uint32_t> legacyInterpolationFps) {
    Setting setting;
    setting.cap = cap && IsSupportedCap(*cap) ? *cap : 0;
    if (mode) {
        setting.mode = IsSupportedMode(*mode) ? *mode : kOff;
        return setting;
    }
    if (legacyInterpolationFps && *legacyInterpolationFps > 60 && IsSupportedCap(*legacyInterpolationFps)) {
        setting.mode = kSmooth;
        setting.cap = *legacyInterpolationFps;
    }
    return setting;
}

// When the presents of a just-sealed frame may start. As soon as it is sealed, but never earlier than
// a quarter millisecond less than one interval after the previous anchor: one slow frame then shifts
// the phase late, and it slides back 0.25 ms per frame instead of jittering. `previous` 0 = none.
inline uint64_t NextPresentAnchor(uint64_t sealNanos, uint64_t previousNanos, uint64_t intervalNanos) {
    constexpr uint64_t kPhaseDecayNanos = 250'000;
    if (previousNanos == 0 || intervalNanos <= kPhaseDecayNanos) {
        return sealNanos;
    }
    const uint64_t earliest = previousNanos + intervalNanos - kPhaseDecayNanos;
    return sealNanos > earliest ? sealNanos : earliest;
}

} // namespace FramePacingSetting
