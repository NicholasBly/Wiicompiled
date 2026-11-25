#include "latency_stats.h"

#include <cmath>
#include <cstdio>
#include <string>

using LatencyStats::Recorder;

namespace {

int g_failures = 0;
constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kBase = 5'000 * kMs;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

bool Near(double actual, double expected) { return std::abs(actual - expected) < 1e-6; }

// One 60 Hz tick: controller read `inputMs` after the tick starts, seal at `sealMs`, first and
// native presents at the given offsets (equal offsets model a frame without slots).
void Frame(Recorder& recorder, uint64_t tag, uint64_t start, double inputMs, double sealMs, double firstMs,
           double nativeMs) {
    const auto at = [start](double ms) { return start + static_cast<uint64_t>(ms * kMs); };
    recorder.OnTickStart(tag, start);
    recorder.OnInputSample(tag, at(inputMs), false);
    recorder.OnSeal(tag, at(sealMs));
    if (firstMs == nativeMs) {
        recorder.OnPresent(tag, at(nativeMs), true, true);
        return;
    }
    recorder.OnPresent(tag, at(firstMs), false, true);
    recorder.OnPresent(tag, at(nativeMs), true, false);
}

void InterpolatedFrameReportsFirstAndNativeLatency() {
    Recorder recorder;
    Frame(recorder, 1, kBase, 0, 3, 4, 15);
    const auto summary = recorder.Summarize();
    Check(summary.inputToFirst.count == 1, "one frame counted for first present");
    Check(Near(summary.inputToFirst.avgMs, 4.0), "controller read -> first present is 4 ms");
    Check(Near(summary.inputToNative.avgMs, 15.0), "controller read -> native present is 15 ms");
    Check(Near(summary.tickCpu.avgMs, 3.0), "tick start -> seal is 3 ms");
}

void FrameWithoutSlotsHasEqualFirstAndNative() {
    Recorder recorder;
    Frame(recorder, 1, kBase, 1, 3, 5, 5);
    const auto summary = recorder.Summarize();
    Check(Near(summary.inputToFirst.avgMs, 4.0), "first present 4 ms after the read");
    Check(Near(summary.inputToNative.avgMs, 4.0), "native present is the same present");
}

void UnknownTagIsIgnored() {
    Recorder recorder;
    recorder.OnPresent(42, kBase, true, true);
    recorder.OnSeal(43, kBase);
    const auto summary = recorder.Summarize();
    Check(summary.inputToNative.count == 0, "events for frames that never started are dropped");
}

void OnlyTheFirstReadOfATickCounts() {
    Recorder recorder;
    recorder.OnTickStart(1, kBase);
    recorder.OnInputSample(1, kBase + 1 * kMs, false);
    recorder.OnInputSample(1, kBase + 2 * kMs, false);
    recorder.OnSeal(1, kBase + 3 * kMs);
    recorder.OnPresent(1, kBase + 6 * kMs, true, true);
    Check(Near(recorder.Summarize().inputToNative.avgMs, 5.0), "latency counts from the first read");
}

void SummaryUsesTheNewestFrames() {
    Recorder recorder;
    for (uint64_t tag = 1; tag <= 300; ++tag) {
        const double native = tag <= 200 ? 50.0 : 5.0;
        Frame(recorder, tag, kBase + tag * 17 * kMs, 0, 2, native, native);
    }
    const auto summary = recorder.Summarize(100);
    Check(summary.inputToNative.count == 100, "summary covers the requested frame count");
    Check(Near(summary.inputToNative.avgMs, 5.0), "only the newest 100 frames are summarized");
}

void PercentilesAndMaximum() {
    Recorder recorder;
    for (uint64_t tag = 1; tag <= 100; ++tag) {
        const double native = static_cast<double>(tag);
        Frame(recorder, tag, kBase + tag * 200 * kMs, 0, 0.5, native, native);
    }
    const auto summary = recorder.Summarize(100);
    Check(Near(summary.inputToNative.avgMs, 50.5), "average of 1..100 ms");
    Check(Near(summary.inputToNative.p99Ms, 99.0), "99th percentile of 1..100 ms");
    Check(Near(summary.inputToNative.maxMs, 100.0), "maximum of 1..100 ms");
}

void PresentIntervalsGiveTheDisplayedRate() {
    Recorder recorder;
    recorder.OnTickStart(1, kBase);
    for (uint64_t i = 0; i < 50; ++i) {
        recorder.OnPresent(1, kBase + i * 4 * kMs, false, i == 0);
    }
    const auto summary = recorder.Summarize();
    Check(summary.presentInterval.count == 49, "49 intervals between 50 presents");
    Check(Near(summary.presentInterval.avgMs, 4.0), "presents 4 ms apart");
    Check(Near(summary.presentFps, 250.0), "4 ms apart is 250 presents per second");
    Check(summary.recentIntervalsMs.size() == 49, "graph holds every interval");
}

void FlashFollowsTheReadOfItsTick() {
    Recorder recorder;
    recorder.OnTickStart(1, kBase);
    recorder.OnInputSample(1, kBase, true);
    recorder.OnTickStart(2, kBase + 17 * kMs);
    recorder.OnInputSample(2, kBase + 17 * kMs, false);
    Check(recorder.FlashHeld(1), "a held button lights the flash for its frame");
    Check(!recorder.FlashHeld(2), "a released button clears it");
}

void CompletedFramesBecomeCsvRowsOnce() {
    Recorder recorder;
    recorder.SetCollectRows(true);
    Frame(recorder, 1, kBase, 0, 3, 4, 15);
    Frame(recorder, 2, kBase + 17 * kMs, 0, 3, 4, 15);
    const auto rows = recorder.TakeCompletedRows();
    Check(rows.size() == 2 && rows[0].tag == 1 && rows[1].tag == 2, "rows come out in frame order");
    Check(recorder.TakeCompletedRows().empty(), "rows are handed out once");
    const std::string line = LatencyStats::CsvRow(rows[0], kBase);
    Check(line == "1,0.000,0.000,3.000,4.000,15.000,4.000,15.000,3.000,0,,,0.000", "CSV row: " + line);
    Check(LatencyStats::CsvHeader().rfind("frame,", 0) == 0, "CSV header starts with frame");
}

void BreakdownShowsWhereTheTickGoes() {
    Recorder recorder;
    recorder.OnTickStart(1, kBase);
    recorder.OnInputSample(1, kBase + 1 * kMs, false);
    recorder.OnDrawDone(1, kBase + 2 * kMs);
    recorder.OnDrawDone(1, kBase + 3 * kMs);
    recorder.AddRetraceWait(1, 5 * kMs);
    recorder.AddRetraceWait(1, 7 * kMs);
    recorder.OnSeal(1, kBase + 15 * kMs);
    recorder.OnPresent(1, kBase + 16 * kMs, true, true);
    const auto summary = recorder.Summarize();
    Check(Near(summary.inputToDrawDone.avgMs, 2.0), "read -> last draw-done is 2 ms");
    Check(Near(summary.drawDoneToSeal.avgMs, 12.0), "last draw-done -> seal is 12 ms");
    Check(Near(summary.retraceWait.avgMs, 12.0), "retrace waits add up to 12 ms");
}

void DrawThreadSleepCountsOnlyAfterTheRead() {
    Recorder recorder;
    recorder.OnTickStart(1, kBase);
    recorder.AddDrawThreadSleep(1, 4 * kMs);   // before the read: not part of read -> drawn
    recorder.OnInputSample(1, kBase + 5 * kMs, false);
    recorder.AddDrawThreadSleep(1, 6 * kMs);
    recorder.AddDrawThreadSleep(1, 3 * kMs);
    recorder.OnSeal(1, kBase + 16 * kMs);
    recorder.AddDrawThreadSleep(1, 9 * kMs);   // after the seal: belongs to no frame
    recorder.OnPresent(1, kBase + 17 * kMs, true, true);
    Check(Near(recorder.Summarize().drawThreadSleep.avgMs, 9.0), "only sleeps between read and seal count");
    recorder.NotePadThread(0x80001000);
    recorder.NoteDrawThread(0x80002000);
    Check(recorder.PadThread() == 0x80001000 && recorder.DrawThread() == 0x80002000, "threads are kept");
}

void WorkBeforeTheWaitIsTickStartToFirstSleep() {
    Recorder recorder;
    recorder.OnTickStart(1, kBase);
    recorder.OnInputSample(1, kBase + 1 * kMs, false);
    recorder.OnDrawThreadSleepStart(1, kBase + 2 * kMs);
    recorder.OnDrawThreadSleepStart(1, kBase + 9 * kMs);   // later sleeps don't move it
    recorder.OnSeal(1, kBase + 16 * kMs);
    recorder.OnPresent(1, kBase + 17 * kMs, true, true);
    Check(Near(recorder.Summarize().workBeforeWait.avgMs, 2.0), "tick start -> first sleep is 2 ms");
}

} // namespace

int main() {
    WorkBeforeTheWaitIsTickStartToFirstSleep();
    DrawThreadSleepCountsOnlyAfterTheRead();
    BreakdownShowsWhereTheTickGoes();
    InterpolatedFrameReportsFirstAndNativeLatency();
    FrameWithoutSlotsHasEqualFirstAndNative();
    UnknownTagIsIgnored();
    OnlyTheFirstReadOfATickCounts();
    SummaryUsesTheNewestFrames();
    PercentilesAndMaximum();
    PresentIntervalsGiveTheDisplayedRate();
    FlashFollowsTheReadOfItsTick();
    CompletedFramesBecomeCsvRowsOnce();
    if (g_failures != 0) {
        std::printf("latency_stats_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("latency_stats_tests: all passed\n");
    return 0;
}
