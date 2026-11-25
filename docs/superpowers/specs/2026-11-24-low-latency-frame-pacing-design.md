# Low-latency frame pacing, any frame-rate cap, Reflex, and a latency overlay

Status: approved direction ("Option A"), 2026-11-24.

## Why

The goal is lower input lag. Smoothness at high refresh rates matters, but not at the cost of lag.
The game logic stays a fixed 60 Hz (physics, timers, online and ghosts all depend on it), so the
work is all in how and when frames get shown.

What the code does today (measured by reading it, not by running it):

- The game reads the controller at the start of each 60 Hz tick (`PAD__Read_HLE`), finishes its
  tick in a few ms, and seals the frame in `VI_HLE_PresentFrame`.
- **Interpolation off:** aurora presents the frame as soon as it is encoded. This is already the
  fastest path (~12 ms from controller read to present, plus monitor time).
- **Interpolation on:** `VI_HLE_PresentFrame` anchors the frame's presents to the *next* VI
  boundary, and the native frame is shown last. At 180 fps the new game state starts to show about
  13 ms later than with interpolation off, and is fully shown about 24 ms later.
- In-between frames are evenly spaced fixed slots (`targetFps / 60 - 1`, max 3), so only 120/180/240
  are possible.

## Goals

1. Smoothing that adds as little lag as possible: at most about one extra present period in
   "Smooth" mode, and none in "Responsive" mode.
2. Any frame-rate cap: 120, 144, 165, 180, 200, 240, 360, or "match display".
3. NVIDIA Reflex: Off / On / On + Boost (Windows, D3D12 backend, NVIDIA GPU).
4. A debug overlay that shows frame times and latency so the modes can be compared, plus CSV
   logging and a flash test for measuring real button-to-photon lag with a camera or an LDAT /
   Reflex Analyzer monitor.

## Non-goals

- Running game logic above 60 Hz (rejected as "Option B").
- Reflex on Vulkan/Linux (needs `VK_NV_low_latency2` enabled inside Dawn's device creation).
- AMD Anti-Lag 2 / Intel XeLL (separate SDKs; can follow later on the same marker hooks).

## Anti-goals (would make this a failure even if it "works")

- Any change to game behavior, VI retrace cadence, audio timing, or online/ghost determinism.
- Making the interpolation-off path slower or less stable than today.

## Approaches considered (for the smoothing schedule)

1. **Keep fixed slots, just anchor them to the seal time.** Cheapest, and it removes most of the
   added lag, but it still can't do 144/165.
2. **Per-frame present plan (chosen).** At seal time, compute the exact present times and blend
   weights for this frame from (mode, anchor, 60 Hz interval, present period). Supports any cap and
   both modes with one pure, unit-testable function.
3. **A free-running present clock decoupled from frames.** It would give the smoothest result on
   fixed-refresh monitors, but it needs vblank-phase tracking that Dawn doesn't expose, and it adds
   latency. Rejected.

## Design

### Present plan (aurora, `lib/gfx/present_plan.{hpp,cpp}`)

Inputs: mode, anchor `A` (ns), native interval `I` (ns, ~16.68 ms), present period `C`
(ns, `1e9 / effective cap`), maximum extra slots `S` (latched at frame begin; up to 5, raised from 3).

- **Smooth (interpolate):** `k = min(ceil(I / C), S + 1)` presents at `A + j*C` (j = 0..k-1), with
  weight `w_j = 1 - (k-1-j) * C / I`. The last present is the native frame (w = 1). Shown game time
  is exactly linear in present time. For integer ratios (120/180/240/360) this is the old even
  spacing. For 144/165 the last gap of each frame is shorter, which looks right on VRR displays.
- **Responsive (predict):** the native frame is shown at `A` (w = 1), then extrapolated slots at
  `A + j*C` with `w_j = 1 + j*C / I` while `j*C < I - C/4` (and j <= S). The newest state is on
  screen as soon as it exists. Cost: a brief overshoot when something suddenly stops or turns.
- Effective cap: `min(user cap, display refresh)` when VSync is on. "Match display" uses the
  display refresh. If the adaptive slot controller lowers the slot count, `C` widens to
  `I / (slots + 1)`.
- Weights are computed in `finalize_frame_interpolation` from the plan (replacing the fixed
  `numerator/denominator`), and the plan is handed to the render worker in `InterpolationReplay`.
  The present loop in `aurora.cpp` walks the plan in order. The native encoder is always submitted,
  even in the rare plan where it isn't the one shown.
- Extrapolation reuses `evaluate_affine_pair` with weights above 1 (already linear in the weight;
  rotations are guaranteed within 45 degrees per frame by the existing validity gates).

### Anchor (runtime, `vi.cpp`)

When smoothing is on, the anchor becomes the seal time, phase-locked:
`anchor = max(now, previousAnchor + interval - 0.25 ms)`. Presents start as soon as the frame
exists. A slow frame shifts the phase late, and it slides back by 0.25 ms per frame instead of
jittering. The VI timeline, retrace delivery and producer pacing are unchanged.

### Settings (runtime, F10 Graphics tab)

- "Smooth frames": Off / Smooth (interpolate) / Responsive (predict). Replaces "Race frame
  interpolation". The old `FrameInterpolationFps` config migrates (120/180 -> Smooth with that cap).
- "Frame rate cap": Match display / 120 / 144 / 165 / 180 / 200 / 240 / 360.
- "NVIDIA Reflex": Off / On / On + Boost. Disabled with a reason when unsupported.
- "Latency overlay" checkbox, and buttons for "Record CSV" and "Flash test".
- New config keys: `FramePacingMode`, `FrameRateCap`, `ReflexMode`, `LatencyOverlay`.
- Exclusive fullscreen picks the refresh closest to the effective cap.

### Frame timing hooks (aurora public API)

- `aurora_set_frame_tag(uint64_t)`: the producer tags the frame it is about to end.
- `aurora_set_frame_timing_callback(cb, user)`: render worker events for each frame: render submit
  start/end, and present start/end for every present, flagged native/first.
- `aurora_set_frame_pacing(mode, capFps)` and `aurora_get_frame_pacing(...)`. The old
  `aurora_set_frame_interpolation_fps` keeps working (maps to Smooth with that cap).
- `aurora_get_adapter_ids(vendor, device)` for Reflex adapter matching.

### Latency overlay (runtime, `latency_stats.{h,cpp}` + overlay drawing)

Per tagged frame: tick start, first controller read, seal, first present, native present.
Rolling window (~2 s):

- Controller read -> first present (when the new state starts to show).
- Controller read -> native present (when the new state is fully shown).
- Game tick CPU time.
- Present interval average, 1% high and max, with a frame-time graph.
- Reflex numbers when Reflex is on (`NvAPI_D3D_GetLatency`: sim start -> GPU render end).
- A header line with mode, cap, effective present rate, VSync, Reflex state and display refresh.
- **CSV recording:** one row per frame, written to the log folder, so Off / On / On + Boost and
  different caps can be compared afterwards.
- **Flash test:** while enabled, a press of A turns a white square on in the corner, on the first
  frame built from that controller read. It also fires Reflex `TRIGGER_FLASH`. This measures the
  pipeline (read -> photon), not the game's own reaction.
- The numbers start at the controller read, not the physical press. Waiting for the next 60 Hz
  read adds 0–16.7 ms (8.3 ms on average) on top, and the overlay says so.

### Reflex (runtime, `nvidia_reflex.{h,cpp}`, Windows only, no-op elsewhere)

- Device: D3D12 devices are singletons per adapter, so `D3D12CreateDevice` on the DXGI adapter
  matching Dawn's vendor/device ID returns Dawn's own device. Dawn's C++ `GetD3D12Device` is not
  callable (MSVC ABI vs. our LLVM-MinGW build).
- NVAPI is loaded at runtime (`nvapi64.dll` -> `nvapi_QueryInterface` by function ID). No import
  library is needed. The few struct layouts are copied from NVIDIA's MIT-licensed `nvapi.h`, with
  its notice.
- `NvAPI_D3D_SetSleepMode` on setting change. `NvAPI_D3D_Sleep` + `SIMULATION_START` at tick start
  (end of `VI_HLE_PresentFrame`), `INPUT_SAMPLE` at the first `PAD__Read_HLE` of the tick,
  `SIMULATION_END` at seal, and `RENDERSUBMIT_*` / `PRESENT_*` from the aurora timing callback for
  the native present. Frame ID = frame tag.
- Supported only when the backend is D3D12, the vendor is NVIDIA, and NVAPI initializes.

## Delivery

- Two repos: `aurora-wiicompiled` (fork) and `Wiicompiled` (fork), branch `low-latency-pacing` in
  both. The WiiCompiled branch points the `aurora-main` submodule at the aurora fork commit.
- Split into reviewable commits: (1) aurora timing hooks + overlay, (2) present plan + modes + caps,
  (3) Reflex. Upstream wants small PRs, and every line has to be explainable by the submitter.
- Verification here: unit tests for the present plan and the latency stats, plus compiling aurora
  and the runtime library on Linux. In-game testing happens on the user's Windows machine, with a
  step-by-step guide in `docs/testing-low-latency.md`.

## Left to the builder

Overlay layout details, CSV column order, the exact rolling-window length, and log wording.
