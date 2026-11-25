#include "nvidia_reflex.h"

#include <atomic>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>

#include <aurora/aurora.h>
#endif

namespace Reflex {

using namespace nvapi_reflex;

NV_SET_SLEEP_MODE_PARAMS SleepParamsFor(Mode mode) {
    NV_SET_SLEEP_MODE_PARAMS params{};
    params.version = kSetSleepModeVersion;
    params.bLowLatencyMode = mode != Mode::Off;
    params.bLowLatencyBoost = mode == Mode::Boost;
    // The VI timeline paces the game; a Reflex frame limit on top would fight it.
    params.minimumIntervalUs = 0;
    return params;
}

namespace {

std::atomic<uint32_t> g_mode{static_cast<uint32_t>(Mode::Off)};
std::atomic<uint64_t> g_lastInputFrame{0};

#if defined(_WIN32)

struct Api {
    using SetSleepModeFn = NvAPI_Status(__cdecl*)(IUnknown*, NV_SET_SLEEP_MODE_PARAMS*);
    using SleepFn = NvAPI_Status(__cdecl*)(IUnknown*);
    using SetLatencyMarkerFn = NvAPI_Status(__cdecl*)(IUnknown*, NV_LATENCY_MARKER_PARAMS*);

    SetSleepModeFn setSleepMode = nullptr;
    SleepFn sleep = nullptr;
    SetLatencyMarkerFn setLatencyMarker = nullptr;
    // Dawn's device. Held for the process lifetime.
    IUnknown* device = nullptr;
};

std::mutex g_initMutex;
bool g_initFinished = false;
const char* g_reason = "Reflex starts once the GPU is set up.";
Api g_api;
std::atomic<bool> g_ready{false};

template <typename Fn>
Fn ProcAs(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// D3D12 devices are singletons per adapter: D3D12CreateDevice on the adapter Dawn renders with
// returns Dawn's own device (Dawn creates it the same way). Dawn's C++ GetD3D12Device is not
// callable from this LLVM-MinGW build, since Dawn ships built with MSVC.
IUnknown* AcquireD3D12Device(uint32_t vendorId, uint32_t deviceId) {
    const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (dxgi == nullptr || d3d12 == nullptr) {
        return nullptr;
    }
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    const auto createFactory = ProcAs<CreateFactoryFn>(dxgi, "CreateDXGIFactory1");
    const auto createDevice = ProcAs<PFN_D3D12_CREATE_DEVICE>(d3d12, "D3D12CreateDevice");
    if (createFactory == nullptr || createDevice == nullptr) {
        return nullptr;
    }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    ID3D12Device* device = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; device == nullptr && factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            desc.VendorId == vendorId && desc.DeviceId == deviceId) {
            createDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
        }
        adapter->Release();
    }
    factory->Release();
    return device;
}

// Sets g_reason; returns whether Reflex can run. Retries until Aurora has a GPU, then decides once.
bool EnsureInitialized() {
    if (g_ready.load(std::memory_order_acquire)) {
        return true;
    }
    std::lock_guard lock(g_initMutex);
    if (g_initFinished) {
        return g_ready.load(std::memory_order_relaxed);
    }
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    AuroraBackend backend = BACKEND_AUTO;
    if (!aurora_get_adapter_info(&vendorId, &deviceId, &backend)) {
        return false;
    }
    g_initFinished = true;
    if (backend != BACKEND_D3D12) {
        g_reason = "Reflex needs the D3D12 graphics API.";
        return false;
    }
    if (vendorId != 0x10DE) {
        g_reason = "Reflex needs an NVIDIA GPU.";
        return false;
    }
    const HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    using QueryInterfaceFn = void*(__cdecl*)(NvU32);
    const auto query = nvapi != nullptr ? ProcAs<QueryInterfaceFn>(nvapi, "nvapi_QueryInterface") : nullptr;
    if (query == nullptr) {
        g_reason = "The NVIDIA driver's NVAPI was not found.";
        return false;
    }
    using InitializeFn = NvAPI_Status(__cdecl*)();
    const auto initialize = reinterpret_cast<InitializeFn>(query(kIdInitialize));
    if (initialize == nullptr || initialize() != NVAPI_OK) {
        g_reason = "NVAPI did not start.";
        return false;
    }
    Api api;
    api.setSleepMode = reinterpret_cast<Api::SetSleepModeFn>(query(kIdD3DSetSleepMode));
    api.sleep = reinterpret_cast<Api::SleepFn>(query(kIdD3DSleep));
    api.setLatencyMarker = reinterpret_cast<Api::SetLatencyMarkerFn>(query(kIdD3DSetLatencyMarker));
    if (api.setSleepMode == nullptr || api.sleep == nullptr || api.setLatencyMarker == nullptr) {
        g_reason = "This NVIDIA driver has no Reflex support.";
        return false;
    }
    api.device = AcquireD3D12Device(vendorId, deviceId);
    if (api.device == nullptr) {
        g_reason = "Could not reach the game's D3D12 device.";
        return false;
    }
    g_api = api;
    g_reason = nullptr;
    g_ready.store(true, std::memory_order_release);
    return true;
}

bool Active() {
    return g_ready.load(std::memory_order_acquire) && g_mode.load(std::memory_order_acquire) != 0;
}

void Mark(uint64_t frame, NV_LATENCY_MARKER_TYPE type) {
    if (!Active() || frame == 0) {
        return;
    }
    NV_LATENCY_MARKER_PARAMS params{};
    params.version = kLatencyMarkerVersion;
    params.frameID = frame;
    params.markerType = type;
    g_api.setLatencyMarker(g_api.device, &params);
}

#else

constexpr const char* kUnsupportedHere = "Reflex needs Windows with the D3D12 graphics API.";

#endif

} // namespace

const char* UnsupportedReason() {
#if defined(_WIN32)
    EnsureInitialized();
    std::lock_guard lock(g_initMutex);
    return g_reason;
#else
    return kUnsupportedHere;
#endif
}

void SetMode(Mode mode) {
#if defined(_WIN32)
    if (!EnsureInitialized()) {
        g_mode.store(static_cast<uint32_t>(Mode::Off), std::memory_order_release);
        return;
    }
    NV_SET_SLEEP_MODE_PARAMS params = SleepParamsFor(mode);
    if (g_api.setSleepMode(g_api.device, &params) != NVAPI_OK) {
        mode = Mode::Off;
    }
    g_mode.store(static_cast<uint32_t>(mode), std::memory_order_release);
#else
    (void)mode;
    g_mode.store(static_cast<uint32_t>(Mode::Off), std::memory_order_release);
#endif
}

Mode CurrentMode() { return static_cast<Mode>(g_mode.load(std::memory_order_acquire)); }

void OnTickStart(uint64_t frame) {
#if defined(_WIN32)
    if (!Active()) {
        return;
    }
    // Reflex holds the CPU here when the GPU is behind, so the tick, and its controller read,
    // starts as late as it can and still finish in time.
    g_api.sleep(g_api.device);
    Mark(frame, SIMULATION_START);
#else
    (void)frame;
#endif
}

void OnInputSample(uint64_t frame) {
    if (g_lastInputFrame.exchange(frame, std::memory_order_acq_rel) == frame) {
        return;
    }
#if defined(_WIN32)
    Mark(frame, INPUT_SAMPLE);
#endif
}

void OnSimulationEnd(uint64_t frame) {
#if defined(_WIN32)
    Mark(frame, SIMULATION_END);
#else
    (void)frame;
#endif
}

void OnFrameTiming(uint64_t frame, uint32_t auroraType) {
#if defined(_WIN32)
    // AuroraFrameTimingType: render submit start/end, present start/end.
    static constexpr NV_LATENCY_MARKER_TYPE kMarkers[] = {RENDERSUBMIT_START, RENDERSUBMIT_END, PRESENT_START,
                                                          PRESENT_END};
    if (auroraType < 4) {
        Mark(frame, kMarkers[auroraType]);
    }
#else
    (void)frame;
    (void)auroraType;
#endif
}

} // namespace Reflex
