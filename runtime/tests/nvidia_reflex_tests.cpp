#include "nvidia_reflex.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool Near(double actual, double expected) { return std::abs(actual - expected) < 1e-9; }

void SleepParamsFollowTheMode() {
    using Reflex::Mode;
    const auto off = Reflex::SleepParamsFor(Mode::Off);
    const auto on = Reflex::SleepParamsFor(Mode::On);
    const auto boost = Reflex::SleepParamsFor(Mode::Boost);
    Check(off.version == nvapi_reflex::kSetSleepModeVersion, "params carry the NVAPI struct version");
    Check(!off.bLowLatencyMode && !off.bLowLatencyBoost, "off asks for nothing");
    Check(on.bLowLatencyMode && !on.bLowLatencyBoost, "on enables low latency only");
    Check(boost.bLowLatencyMode && boost.bLowLatencyBoost, "boost also keeps GPU clocks up");
    Check(on.minimumIntervalUs == 0, "Reflex never caps the frame rate here; VI pacing does");
}

void LatencyReportAveragesRenderedFrames() {
    nvapi_reflex::NV_LATENCY_RESULT_PARAMS result{};
    auto& a = result.frameReport[10];
    a.frameID = 100;
    a.inputSampleTime = 1'000;
    a.simStartTime = 1'000;
    a.gpuRenderEndTime = 9'000;
    a.gpuActiveRenderTimeUs = 2'000;
    auto& b = result.frameReport[11];
    b.frameID = 101;
    b.simStartTime = 20'000;
    b.gpuRenderEndTime = 30'000;
    b.gpuActiveRenderTimeUs = 4'000;
    auto& pending = result.frameReport[12];
    pending.frameID = 102;
    pending.simStartTime = 40'000;
    const auto report = Reflex::SummarizeLatency(result);
    Check(report.frames == 2, "unrendered and empty reports are skipped");
    Check(Near(report.simToGpuEndMs, 9.0), "sim start -> GPU done averages 8 and 10 ms");
    Check(Near(report.inputToGpuEndMs, 8.0), "only frames with an input sample count for input latency");
    Check(Near(report.gpuActiveMs, 3.0), "GPU busy time averages 2 and 4 ms");
}

void ReflexIsUnavailableOffWindows() {
#if !defined(_WIN32)
    Check(Reflex::UnsupportedReason() != nullptr, "Reflex reports why it cannot run");
    Reflex::SetMode(Reflex::Mode::Boost);
    Check(Reflex::CurrentMode() == Reflex::Mode::Off, "an unsupported Reflex stays off");
#endif
}

} // namespace

int main() {
    SleepParamsFollowTheMode();
    LatencyReportAveragesRenderedFrames();
    ReflexIsUnavailableOffWindows();
    if (g_failures != 0) {
        std::printf("nvidia_reflex_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("nvidia_reflex_tests: all passed\n");
    return 0;
}
