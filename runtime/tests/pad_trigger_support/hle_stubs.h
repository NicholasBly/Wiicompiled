#pragma once

#include <cstdint>

#define PPC_NATIVE_OVERRIDE(...)
#define PPC_NATIVE_OVERRIDE_VOID(...)

// pad.cpp tags its controller read with the current frame for the latency overlay.
inline uint64_t VI_HLE_CurrentFrameTag() { return 0; }
inline uint32_t VI_HLE_CurrentGuestThread() { return 0; }
