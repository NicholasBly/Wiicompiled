# Low-latency Frame Pacing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Smoothing that doesn't add lag, at any frame-rate cap, plus NVIDIA Reflex and a latency/frame-time overlay, with game logic left at 60 Hz.

**Architecture:** A pure per-frame present plan in aurora decides present times and blend weights. The runtime anchors plans to the seal time, drives the settings, records latency from aurora timing callbacks, and drives Reflex through runtime-loaded NVAPI on Dawn's D3D12 device.

**Tech Stack:** C++20, aurora (Dawn/WebGPU, ImGui), SDL3, CMake/Ninja, LLVM-MinGW on Windows.

**Spec:** `docs/superpowers/specs/2026-11-24-low-latency-frame-pacing-design.md`

## Global Constraints

- Game logic, VI retrace cadence, audio and alarms: unchanged.
- The interpolation-off path must present exactly as it does today (no new waits).
- Max extra slots `MaxInterpolatedFrames` = 5. Caps: 0 (match display), 120, 144, 165, 180, 200, 240, 360.
- Anchor phase-lock decay: 0.25 ms per frame.
- Reflex: Windows + D3D12 backend + vendor 0x10DE only; everywhere else it's a compiled no-op.
- NVAPI structs are copied from `nvapi.h` (MIT), with the notice. No link-time NVAPI dependency.

## Review Focus

- A late frame (seal after its window): the plan must still present the native frame, and must not burst every slot.
- Cap at or below 60, or above the display refresh with VSync on: no slots, or the refresh-limited period.
- Switching mode or cap mid-race: there must be no stale weights from the previous config.
- Reflex selected on a non-NVIDIA GPU, or on Vulkan: the UI shows why, and no NVAPI calls happen.
- Overlay with interpolation off: first present = native present, and the numbers stay valid.

---

### Task 1: Present plan (aurora)

**Files:** Create `aurora-main/lib/gfx/present_plan.{hpp,cpp}`, `aurora-main/tests/present_plan_test.cpp`; modify aurora `tests/CMakeLists.txt`, `cmake/aurora_gx.cmake`.

**Interfaces — Produces:**
`enum class PacingMode : uint32_t { Off = 0, Smooth = 1, Responsive = 2 };`
`struct PlannedPresent { uint64_t timeNanos; float weight; int32_t slot; /* -1 = native */ };`
`struct PresentPlan { std::array<PlannedPresent, MaxInterpolatedFrames + 1> presents; uint32_t count; };`
`PresentPlan make_present_plan(PacingMode, uint64_t anchor, uint64_t interval, uint64_t period, uint32_t maxSlots) noexcept;`
`uint32_t slots_needed(PacingMode, uint64_t interval, uint64_t period) noexcept;`

- [ ] Tests: 180 fps Smooth → 3 presents at A, A+I/3, A+2I/3 with weights 1/3, 2/3, native. 144 Smooth → 3 presents, weights 1-2C/I, 1-C/I, native. Responsive 180 → native at A, then 4/3 and 5/3. maxSlots=1 at 240 → period widens to I/2. Off or period ≥ I → native only at time 0. `slots_needed`: 180→2, 144→2, 360→5, 120→1.
- [ ] Implement per spec formulas; run `ctest -R present_plan`; commit.

### Task 2: Plan-driven interpolation + modes/caps (aurora)

**Files:** Modify `lib/gx/frame_interpolation.{hpp,cpp}`, `lib/gfx/types.hpp` (`InterpolationReplay` gains `PresentPlan plan`), `lib/gfx/frame.cpp`, `lib/aurora.cpp` (present loop walks the plan), `lib/gfx/presentation.cpp`, `lib/window.cpp` (fullscreen refresh), `include/aurora/gfx.h`.

**Interfaces — Produces (C API):** `aurora_set_frame_pacing(uint32_t mode, uint32_t capFps)`, `aurora_get_frame_pacing(uint32_t* mode, uint32_t* capFps)`, `uint32_t aurora_get_effective_present_fps()`. `aurora_set_frame_interpolation_fps(fps)` maps to Smooth with that cap.

- [ ] The slot count latched at begin = `min(adaptive target, slots_needed)`. The plan is computed in `finalize_frame_interpolation` from `present_schedule()`, and each task's weight is read from the plan by slot index. Tasks for slots that aren't used are skipped.
- [ ] Present loop: in plan order, `wait_until(time)`, encode the slot or use the native encoder. The native encoder is always submitted. Keep the existing skip rule for slots whose successor is already due.
- [ ] Build `aurora_gx`; commit.

### Task 3: Frame timing hooks + adapter IDs (aurora)

**Files:** Modify `include/aurora/aurora.h`, `lib/aurora.cpp`, `lib/gfx/frame.cpp`, `lib/webgpu/gpu.cpp`.

**Interfaces — Produces:**
`typedef enum { AURORA_FRAME_RENDER_SUBMIT_START, AURORA_FRAME_RENDER_SUBMIT_END, AURORA_FRAME_PRESENT_START, AURORA_FRAME_PRESENT_END } AuroraFrameTimingType;`
`typedef struct { uint64_t frameTag; uint64_t timeNanos; AuroraFrameTimingType type; int32_t slot; bool native; bool first; } AuroraFrameTimingEvent;`
`void aurora_set_frame_timing_callback(void (*)(const AuroraFrameTimingEvent*, void*), void* user);`
`void aurora_set_frame_tag(uint64_t tag);` (latched into the packet at end_frame)
`bool aurora_get_adapter_ids(uint32_t* vendorId, uint32_t* deviceId);`

- [ ] Fire events from the render worker around the native submit and around every `Present()`. `timeNanos` uses `presentation::now_nanos()` (steady_clock). Build; commit.

### Task 4: Latency stats + overlay + CSV + flash (runtime)

**Files:** Create `runtime/include/latency_stats.h`, `runtime/src/latency_stats.cpp`, `runtime/tests/latency_stats_tests.cpp`; modify `runtime/cmake/PlatformTests.cmake`, `runtime/src/hle/input/pad.cpp`, `runtime/src/hle/vi.cpp`, `runtime/src/settings_overlay.cpp`.

**Interfaces — Produces (namespace `LatencyStats`):** `OnTickStart(uint64_t ns)`, `OnInputSample(uint64_t ns, bool flashButtonPressed)`, `OnSeal(uint64_t tag, uint64_t ns)`, `OnFrameEvent(const AuroraFrameTimingEvent&)` (thread-safe), `Snapshot Summary()` → `{inputToFirstMs, inputToNativeMs {avg, p99}, tickCpuMs, presentIntervalMs {avg, p99, max}, presentFps, recentIntervals[120]}`, `StartCsv(path)/StopCsv()`, `bool FlashActive(uint64_t tag)`.

- [ ] Tests: a frame with input at 0, seal at 3 ms, first present at 4 ms, native at 15 ms gives 4 / 15. Off-mode frame (first == native). An event for an unknown tag is ignored. Ring wrap keeps the newest 240. Percentiles on known data.
- [ ] Wire the hooks; overlay window behind `g_latencyOverlay`; Graphics tab controls; config key `LatencyOverlay`. Build `mkw_runtime` + run the test; commit.

### Task 5: Settings + anchor (runtime)

**Files:** Modify `runtime/src/hle/vi.cpp` (anchor phase-lock), `runtime/src/settings_overlay.cpp`, runtime config files (`runtime_config*.{h,cpp}`, `game/runtime_config_values.cpp`).

- [ ] Config `FramePacingMode` (0..2), `FrameRateCap` (0 or a listed cap). Migration from `FrameInterpolationFps`. UI combos as in the spec. Apply at startup and on change. Warnings for an inactive cap or uneven pacing without VSync are kept and adapted.
- [ ] Anchor: `max(now, prev + interval - 250us)` when the mode isn't Off. The old next-boundary anchor is removed. Build; commit.

### Task 6: Reflex (runtime)

**Files:** Create `runtime/include/nvidia_reflex.h`, `runtime/src/nvidia_reflex.cpp`, `runtime/third_party/nvapi_reflex/nvapi_reflex.h` (+ `LICENSE`); modify vi.cpp, pad.cpp, settings_overlay.cpp, config.

**Interfaces — Produces (namespace `Reflex`):** `enum class Mode { Off, On, Boost }`, `bool Supported(std::string* reason)`, `void SetMode(Mode)`, `void OnTickStart(uint64_t frameId)` (Sleep + SIMULATION_START), `void Marker(uint64_t frameId, uint32_t type)`, `bool LatestLatency(ReflexLatency&)`.

- [ ] Windows body: lazy init after aurora init (adapter match → `D3D12CreateDevice` singleton → `nvapi_QueryInterface`). Non-Windows: stubs returning unsupported. Compile-check with a MinGW-style `-target x86_64-w64-windows-gnu -fsyntax-only` if the headers are available, otherwise by review. Commit.

### Task 7: Testing guide + delivery

**Files:** Create `docs/testing-low-latency.md`; modify `.gitmodules` (aurora fork URL), the submodule pointer.

- [ ] A Windows from-source guide (translator → shards → cmake → run), and a test matrix (Off / Smooth / Responsive × caps × Reflex). Push both forks; full compile check; commit.
