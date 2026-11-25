#pragma once

#include <cstdint>

#include "../third_party/nvapi_reflex/nvapi_reflex.h"

// NVIDIA Reflex low-latency mode, on Windows with the D3D12 backend and an NVIDIA GPU. Everywhere
// else every call is a no-op and UnsupportedReason says why.
namespace Reflex {

enum class Mode : uint32_t {
    Off = 0,
    On = 1,
    // Also keeps GPU clocks up, which helps a game as light as this one most.
    Boost = 2,
};

nvapi_reflex::NV_SET_SLEEP_MODE_PARAMS SleepParamsFor(Mode mode);

// nullptr when Reflex can run. Valid after Aurora has created its GPU device.
const char* UnsupportedReason();
void SetMode(Mode mode);
Mode CurrentMode();

// Frame markers, keyed by the frame tag (see VI_HLE_CurrentFrameTag).
// Tick start: Reflex's sleep, then SIMULATION_START.
void OnTickStart(uint64_t frame);
// The first controller read of the tick.
void OnInputSample(uint64_t frame);
void OnSimulationEnd(uint64_t frame);
// Aurora AuroraFrameTimingType for the native frame (render submit and present pairs).
void OnFrameTiming(uint64_t frame, uint32_t auroraType);

} // namespace Reflex
