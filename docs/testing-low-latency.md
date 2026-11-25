# Testing low-latency frame pacing and Reflex

This branch (`low-latency-pacing`) adds:

- **Smooth frames**: Off / Smooth (interpolate) / Responsive (predict), with a **frame rate cap**
  (match display, 120, 144, 165, 180, 200, 240, 360). The game itself still runs at 60.
- **NVIDIA Reflex**: Off / On / On + Boost (Windows, D3D12, NVIDIA GPU).
- A **latency overlay** with CSV recording and a flash test, to compare all of the above.

Everything is in **F10 → Graphics**.

## 1. Get the code

The branch needs the matching aurora commits, which live in your aurora fork
(`NicholasBly/aurora-wiicompiled`, branch `low-latency-pacing`).

```powershell
git clone --branch low-latency-pacing https://github.com/NicholasBly/Wiicompiled.git
cd Wiicompiled
git submodule update --init aurora-main
git -C aurora-main log --oneline -3   # should show "Report per-frame render and present timing..."
```

If `git submodule update` says the commit is not found, the aurora fork hasn't been pushed yet.
Apply the patches instead (they're attached to the chat as `aurora-low-latency-pacing.patch`):

```powershell
git -C aurora-main checkout 5ba1388
git -C aurora-main am ..\aurora-low-latency-pacing.patch
```

## 2. Build

Build it the same way you build your other branches (for example `Launcher\LocalBuild.ps1` with
`-Workspace` pointing at this checkout). Because `aurora-main` differs from the published prebuilt
package, the build compiles aurora from source automatically. That takes longer the first time.

The general from-source steps are in [`translator/README.md`](../translator/README.md), and the
[macOS guide](building-macos.md) shows the same sequence in full.

For Reflex, use the D3D12 graphics API. "Auto" is D3D12 on Windows, or set it explicitly in
`%LOCALAPPDATA%\WiiCompiled\Config.toml`:

```toml
[video]
graphics_api = "d3d12"
```

## 3. Turn on the overlay

F10 → Graphics → **Latency overlay**. The top-left readout shows:

| Line | Meaning |
|---|---|
| Pacing / cap / aiming / VSync / Reflex / display | The settings in effect, and the present rate the pacing is aiming for. |
| Shown | Presents per second that actually reached the display. |
| Read → first frame | Controller read → the first present that starts showing that tick. |
| Read → full frame | Controller read → the present of the real (native) frame. |
| Game tick (CPU) | How long the game itself took for the tick. |
| Frame time | Time between presents, with a graph. Flat is good, spikes are stutter. |
| Reflex (driver) | NVIDIA's own numbers, only while Reflex is on. |

These start at the moment the game reads the controller. On top of that there's the wait for that
read (0–16.7 ms, 8.3 ms on average, because the game reads input 60 times a second), plus
controller, USB and monitor time. Those parts are the same in every mode, so comparisons stay fair.

## 4. Test matrix

Race the same track for about 30 s per row, in Time Trial so nothing else changes. Press
**Record CSV** at the start of each row and **Stop CSV** at the end. Files go to
`%LOCALAPPDATA%\WiiCompiled\LatencyLogs\`, and their names include the mode, cap, VSync and Reflex
setting.

| # | Smooth frames | Cap | Reflex | What to expect |
|---|---|---|---|---|
| 1 | Off | – | Off | Baseline. First = full, a few ms. |
| 2 | Off | – | On | About the same as 1; maybe slightly lower GPU time. |
| 3 | Off | – | On + Boost | Lowest "full frame" of the Off rows (GPU clocks stay up). |
| 4 | Smooth | 120 / 144 / 165 / 240 | Off | First ≈ row 1; full frame about 8–14 ms later than row 1. |
| 5 | Responsive | same caps | Off | First = full ≈ row 1. Watch for overshoot when hitting walls. |
| 6 | Responsive | your refresh | On + Boost | Expected best overall. |
| 7 | Smooth | 360 | Off | Stress test: does "Shown" reach 360? Any stutter in the graph? |

Also check:

- **VSync on vs off** at 144/165. With G-Sync/FreeSync, VSync on should still feel fine.
- **A 60 Hz display**: Smooth frames should say they have no effect, and nothing should change.
- **Changing modes mid-race**: there should be no hitch longer than a frame or two.

## 5. Flash test (real button-to-photon lag)

Tick **Flash test (hold A)**. While A is held, a white square appears in the top-left corner, on
the first frame built from the controller read that saw the press. Film the controller and the
corner with a phone in slow motion (240 fps is plenty) and count frames from the button going down
to the square appearing. An LDAT or a Reflex Latency Analyzer monitor works too: the square fires
Reflex's flash marker.

## 6. Things to report back

- The CSVs (or the overlay numbers) for each row above.
- Anything that looks wrong in Smooth or Responsive: objects jumping, wobbling, or overshooting.
- Whether the Reflex combo is enabled, and if not, the reason shown under it.
- `console.log` from `%LOCALAPPDATA%\WiiCompiled\Logs\<latest run>\` if anything crashes.

## Known limits

- Reflex is only for D3D12 and NVIDIA. On Vulkan it stays off.
- The game reads input 60 times a second, so none of this removes that 0–16.7 ms wait.
- Responsive mode guesses one frame ahead, so things that stop or turn suddenly can overshoot
  for a moment.
- 144/165 caps look best with G-Sync/FreeSync. 120/180/240/360 divide evenly into 60 and pace
  perfectly on any display.
