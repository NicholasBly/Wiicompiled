#pragma once

#include <cstdint>

#define PPC_NATIVE_OVERRIDE(...)
#define PPC_NATIVE_OVERRIDE_VOID(...)

// pad.cpp tags its controller read with the current frame for Reflex.
inline uint64_t VI_HLE_CurrentFrameTag() { return 0; }
