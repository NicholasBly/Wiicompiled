#include "frame_pacing_setting.h"

#include <cstdio>
#include <optional>
#include <string>

using FramePacingSetting::NextPresentAnchor;
using FramePacingSetting::Resolve;
using FramePacingSetting::Setting;

namespace {

int g_failures = 0;

void Expect(const Setting& actual, uint32_t mode, uint32_t cap, const std::string& what) {
    if (actual.mode != mode || actual.cap != cap) {
        std::printf("  FAIL: %s (got mode %u cap %u)\n", what.c_str(), actual.mode, actual.cap);
        ++g_failures;
    }
}

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

constexpr uint64_t kUs = 1'000;
constexpr uint64_t kInterval = 16'666 * kUs;

void AnchorFollowsTheSealTime() {
    const uint64_t first = 1'000'000 * kUs;
    Check(NextPresentAnchor(first, 0, kInterval) == first, "the first anchor is the seal time");
    const uint64_t onTime = first + kInterval + 100 * kUs;
    Check(NextPresentAnchor(onTime, first, kInterval) == onTime, "an on-time frame presents at once");
    const uint64_t early = first + kInterval - 1'000 * kUs;
    Check(NextPresentAnchor(early, first, kInterval) == first + kInterval - 250 * kUs,
          "an early frame waits until 0.25 ms before the previous phase");
}

void AnchorPhaseSlidesBackAfterASlowFrame() {
    // Frames seal at base + k * interval, except frame 1, which seals 2 ms late.
    const uint64_t base = 1'000'000 * kUs;
    uint64_t anchor = base;
    uint64_t seal = base + kInterval + 2'000 * kUs;
    anchor = NextPresentAnchor(seal, anchor, kInterval);
    Check(anchor == seal, "a late frame presents when it is ready");
    for (uint64_t frame = 2; frame <= 5; ++frame) {
        seal = base + frame * kInterval;
        anchor = NextPresentAnchor(seal, anchor, kInterval);
    }
    // Back on the old phase, the anchor drops 0.25 ms per frame instead of snapping back.
    Check(anchor > seal, "the anchor stays on the late phase for a while");
    Check(anchor - seal == 1'000 * kUs, "four frames later it has slid back 1 ms");
}

} // namespace

int main() {
    AnchorFollowsTheSealTime();
    AnchorPhaseSlidesBackAfterASlowFrame();
    using std::nullopt;
    Expect(Resolve(nullopt, nullopt, nullopt), 0, 0, "nothing saved is off, matching the display");
    Expect(Resolve(nullopt, nullopt, 120u), 1, 120, "old 120 fps interpolation becomes smooth at 120");
    Expect(Resolve(nullopt, nullopt, 180u), 1, 180, "old 180 fps interpolation becomes smooth at 180");
    Expect(Resolve(nullopt, nullopt, 240u), 1, 240, "an early build's 240 is supported again");
    Expect(Resolve(nullopt, nullopt, 0u), 0, 0, "old interpolation off stays off");
    Expect(Resolve(nullopt, nullopt, 150u), 0, 0, "an unknown old target is ignored");
    Expect(Resolve(2u, 144u, 180u), 2, 144, "saved pacing wins over the old key");
    Expect(Resolve(0u, nullopt, 180u), 0, 0, "saved off wins over the old key");
    Expect(Resolve(7u, 144u, nullopt), 0, 144, "an unknown mode is off but keeps the cap");
    Expect(Resolve(1u, 150u, nullopt), 1, 0, "an unknown cap matches the display");
    Expect(Resolve(1u, 360u, nullopt), 1, 360, "360 is a supported cap");
    if (g_failures != 0) {
        std::printf("frame_pacing_setting_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("frame_pacing_setting_tests: all passed\n");
    return 0;
}
