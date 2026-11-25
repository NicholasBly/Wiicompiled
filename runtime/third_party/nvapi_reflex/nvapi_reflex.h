/*
 * The NVIDIA Reflex subset of NVAPI that WiiCompiled uses, copied from NVIDIA's nvapi.h,
 * nvapi_lite_common.h and nvapi_interface.h (https://github.com/NVIDIA/nvapi, R590 "Oct 29, 2026"
 * headers). Only the type and struct definitions and function IDs are taken; the functions are
 * reached at runtime through nvapi64.dll's nvapi_QueryInterface, so no NVAPI library is linked.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE in this directory.
 */
#pragma once

#include <cstdint>

#pragma pack(push, 8) // nvapi.h's packing

namespace nvapi_reflex {

using NvU8 = unsigned char;
using NvU32 = uint32_t;
using NvS32 = int32_t;
using NvU64 = uint64_t;
using NvBool = NvU8;
// NvAPI_Status; NVAPI_OK is 0.
using NvAPI_Status = int;
constexpr NvAPI_Status NVAPI_OK = 0;

constexpr NvU32 MakeVersion(uint32_t size, uint32_t version) { return static_cast<NvU32>(size | (version << 16)); }

struct NV_GET_SLEEP_STATUS_PARAMS {
    NvU32 version;
    NvBool bLowLatencyMode;
    NvBool bFsVrr;
    NvBool bCplVsyncOn;
    NvU32 sleepIntervalUs;
    NvBool bUseGameSleep;
    NvBool bFullscreenIFlip;
    NvU8 fgMultiplier;
    NvBool bDfgControl;
    NvU32 dfgFrameTimeTargetUs;
    NvU8 rsvd[114];
};
constexpr NvU32 kSleepStatusVersion = MakeVersion(sizeof(NV_GET_SLEEP_STATUS_PARAMS), 1);

struct NV_SET_SLEEP_MODE_PARAMS {
    NvU32 version;
    NvBool bLowLatencyMode;
    NvBool bLowLatencyBoost;
    NvU32 minimumIntervalUs;
    NvBool bUseMarkersToOptimize;
    NvBool bUseMinQueueTime;
    NvU8 rsvd[30];
};
constexpr NvU32 kSetSleepModeVersion = MakeVersion(sizeof(NV_SET_SLEEP_MODE_PARAMS), 1);

struct NV_LATENCY_RESULT_PARAMS {
    NvU32 version;
    struct FrameReport {
        NvU64 frameID;
        NvU64 inputSampleTime;
        NvU64 simStartTime;
        NvU64 simEndTime;
        NvU64 renderSubmitStartTime;
        NvU64 renderSubmitEndTime;
        NvU64 presentStartTime;
        NvU64 presentEndTime;
        NvU64 driverStartTime;
        NvU64 driverEndTime;
        NvU64 osRenderQueueStartTime;
        NvU64 osRenderQueueEndTime;
        NvU64 gpuRenderStartTime;
        NvU64 gpuRenderEndTime;
        NvU32 gpuActiveRenderTimeUs;
        NvU32 gpuFrameTimeUs;
        NvU64 cameraConstructedTime;
        NvU32 crossAdapterCopyTimeUs;
        NvU32 aiFrameTimeUs;
        NvU8 rsvd[104];
    } frameReport[64];
    NvU8 rsvd[32];
};
constexpr NvU32 kLatencyResultVersion = MakeVersion(sizeof(NV_LATENCY_RESULT_PARAMS), 1);

enum NV_LATENCY_MARKER_TYPE {
    SIMULATION_START = 0,
    SIMULATION_END = 1,
    RENDERSUBMIT_START = 2,
    RENDERSUBMIT_END = 3,
    PRESENT_START = 4,
    PRESENT_END = 5,
    INPUT_SAMPLE = 6,
    TRIGGER_FLASH = 7,
    PC_LATENCY_PING = 8,
};

struct NV_LATENCY_MARKER_PARAMS {
    NvU32 version;
    NvU64 frameID;
    NV_LATENCY_MARKER_TYPE markerType;
    NvU64 rsvd0;
    NvU8 rsvd[56];
};
constexpr NvU32 kLatencyMarkerVersion = MakeVersion(sizeof(NV_LATENCY_MARKER_PARAMS), 1);

// nvapi_interface.h IDs for nvapi_QueryInterface.
constexpr NvU32 kIdInitialize = 0x0150e828;
constexpr NvU32 kIdD3DGetSleepStatus = 0xaef96ca1;
constexpr NvU32 kIdD3DSetSleepMode = 0xac1ca9e0;
constexpr NvU32 kIdD3DSleep = 0x852cd1d2;
constexpr NvU32 kIdD3DGetLatency = 0x1a587f9c;
constexpr NvU32 kIdD3DSetLatencyMarker = 0xd9984c05;

} // namespace nvapi_reflex

#pragma pack(pop)
