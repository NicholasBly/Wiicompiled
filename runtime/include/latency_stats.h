#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Per-frame latency and frame-time bookkeeping for the latency overlay. The game thread reports a
// tick's start, its first controller read and its seal; the render worker reports presents. All
// times are steady_clock nanoseconds, the clock Aurora's present timing uses.
namespace LatencyStats {

struct Stat {
    double avgMs = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;
    uint32_t count = 0;
};

struct Summary {
    // Controller read -> the first present of that frame (the new state starts to show).
    Stat inputToFirst;
    // Controller read -> the native frame's present (the new state is fully shown).
    Stat inputToNative;
    // Tick start -> seal: the game's own time per frame.
    Stat tickCpu;
    // Controller read -> the tick's last GXDrawDone (the game finished drawing).
    Stat inputToDrawDone;
    // Last GXDrawDone -> seal (GXCopyDisp): how long a finished frame waits to be handed over.
    Stat drawDoneToSeal;
    // Time the game spent inside VIWaitForRetrace during the tick.
    Stat retraceWait;
    // Time between consecutive presents, inserted ones included.
    Stat presentInterval;
    double presentFps = 0.0;
    // Present intervals for the frame-time graph, oldest first.
    std::vector<float> recentIntervalsMs;
};

struct FrameRow {
    uint64_t tag = 0;
    uint64_t tickStartNs = 0;
    uint64_t inputNs = 0;
    uint64_t sealNs = 0;
    uint64_t firstPresentNs = 0;
    uint64_t nativePresentNs = 0;
    uint64_t drawDoneNs = 0;
    uint64_t retraceWaitNs = 0;
    bool flash = false;
};

class Recorder {
public:
    void OnTickStart(uint64_t tag, uint64_t ns);
    // Only the first read of a tick counts. `flashHeld`: the flash-test button is down.
    void OnInputSample(uint64_t tag, uint64_t ns, bool flashHeld);
    void OnSeal(uint64_t tag, uint64_t ns);
    // Every GXDrawDone; the last one before the seal counts.
    void OnDrawDone(uint64_t tag, uint64_t ns);
    // Adds one VIWaitForRetrace call's duration.
    void AddRetraceWait(uint64_t tag, uint64_t durationNs);
    // Every present that reached the display; `first` is the frame's first one.
    void OnPresent(uint64_t tag, uint64_t ns, bool native, bool first);

    // Over the newest `frames` frames whose native frame was presented.
    Summary Summarize(uint32_t frames = 120) const;
    bool FlashHeld(uint64_t tag) const;

    // While collecting, frames whose native present arrived queue up as rows for CSV logging.
    void SetCollectRows(bool collect);
    std::vector<FrameRow> TakeCompletedRows();

private:
    static constexpr size_t kFrames = 256;
    static constexpr size_t kIntervals = 240;

    FrameRow* Find(uint64_t tag);
    const FrameRow* Find(uint64_t tag) const;

    mutable std::mutex m_mutex;
    FrameRow m_frames[kFrames]{};
    // Native-presented frames, newest last, as tags into m_frames.
    std::vector<uint64_t> m_completed;
    float m_intervalsMs[kIntervals]{};
    size_t m_intervalCount = 0;
    size_t m_intervalWrite = 0;
    uint64_t m_lastPresentNs = 0;
    bool m_collectRows = false;
    std::vector<FrameRow> m_rows;
};

// The recorder the runtime reports into.
Recorder& Global();

// steady_clock nanoseconds, the timeline Aurora reports presents on.
uint64_t NowNanos();

std::string CsvHeader();
// Times in milliseconds since `originNs`, then the derived latencies.
std::string CsvRow(const FrameRow& row, uint64_t originNs);

} // namespace LatencyStats
