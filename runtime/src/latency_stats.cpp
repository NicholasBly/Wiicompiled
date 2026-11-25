#include "latency_stats.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace LatencyStats {
namespace {

Stat Describe(std::vector<double>& values) {
    Stat stat;
    if (values.empty()) {
        return stat;
    }
    std::sort(values.begin(), values.end());
    double sum = 0.0;
    for (const double value : values) {
        sum += value;
    }
    stat.count = static_cast<uint32_t>(values.size());
    stat.avgMs = sum / static_cast<double>(values.size());
    stat.p99Ms = values[static_cast<size_t>(std::floor(static_cast<double>(values.size() - 1) * 0.99))];
    stat.maxMs = values.back();
    return stat;
}

double Milliseconds(uint64_t from, uint64_t to) {
    return static_cast<double>(static_cast<int64_t>(to - from)) / 1e6;
}

} // namespace

FrameRow* Recorder::Find(uint64_t tag) {
    FrameRow& frame = m_frames[tag % kFrames];
    return tag != 0 && frame.tag == tag ? &frame : nullptr;
}

const FrameRow* Recorder::Find(uint64_t tag) const {
    const FrameRow& frame = m_frames[tag % kFrames];
    return tag != 0 && frame.tag == tag ? &frame : nullptr;
}

void Recorder::OnTickStart(uint64_t tag, uint64_t ns) {
    std::lock_guard lock(m_mutex);
    m_frames[tag % kFrames] = FrameRow{.tag = tag, .tickStartNs = ns};
}

void Recorder::OnInputSample(uint64_t tag, uint64_t ns, bool flashHeld) {
    std::lock_guard lock(m_mutex);
    if (FrameRow* frame = Find(tag); frame != nullptr && frame->inputNs == 0) {
        frame->inputNs = ns;
        frame->flash = flashHeld;
    }
}

void Recorder::OnSeal(uint64_t tag, uint64_t ns) {
    std::lock_guard lock(m_mutex);
    if (FrameRow* frame = Find(tag)) {
        frame->sealNs = ns;
    }
}

void Recorder::OnPresent(uint64_t tag, uint64_t ns, bool native, bool first) {
    std::lock_guard lock(m_mutex);
    if (m_lastPresentNs != 0 && ns > m_lastPresentNs) {
        m_intervalsMs[m_intervalWrite] = static_cast<float>(Milliseconds(m_lastPresentNs, ns));
        m_intervalWrite = (m_intervalWrite + 1) % kIntervals;
        m_intervalCount = std::min(m_intervalCount + 1, kIntervals);
    }
    m_lastPresentNs = ns;

    FrameRow* frame = Find(tag);
    if (frame == nullptr) {
        return;
    }
    if ((first || native) && frame->firstPresentNs == 0) {
        frame->firstPresentNs = ns;
    }
    if (!native || frame->nativePresentNs != 0) {
        return;
    }
    frame->nativePresentNs = ns;
    m_completed.push_back(tag);
    if (m_completed.size() > kFrames) {
        m_completed.erase(m_completed.begin());
    }
    if (m_collectRows) {
        m_rows.push_back(*frame);
    }
}

Summary Recorder::Summarize(uint32_t frames) const {
    std::lock_guard lock(m_mutex);
    std::vector<double> toFirst;
    std::vector<double> toNative;
    std::vector<double> tickCpu;
    const size_t take = std::min<size_t>(frames, m_completed.size());
    for (size_t i = m_completed.size() - take; i < m_completed.size(); ++i) {
        const FrameRow* frame = Find(m_completed[i]);
        if (frame == nullptr) {
            continue;
        }
        if (frame->inputNs != 0) {
            toFirst.push_back(Milliseconds(frame->inputNs, frame->firstPresentNs));
            toNative.push_back(Milliseconds(frame->inputNs, frame->nativePresentNs));
        }
        if (frame->tickStartNs != 0 && frame->sealNs != 0) {
            tickCpu.push_back(Milliseconds(frame->tickStartNs, frame->sealNs));
        }
    }

    Summary summary;
    summary.inputToFirst = Describe(toFirst);
    summary.inputToNative = Describe(toNative);
    summary.tickCpu = Describe(tickCpu);
    summary.recentIntervalsMs.reserve(m_intervalCount);
    std::vector<double> intervals;
    intervals.reserve(m_intervalCount);
    for (size_t i = 0; i < m_intervalCount; ++i) {
        const float value = m_intervalsMs[(m_intervalWrite + kIntervals - m_intervalCount + i) % kIntervals];
        summary.recentIntervalsMs.push_back(value);
        intervals.push_back(value);
    }
    summary.presentInterval = Describe(intervals);
    summary.presentFps = summary.presentInterval.avgMs > 0.0 ? 1000.0 / summary.presentInterval.avgMs : 0.0;
    return summary;
}

bool Recorder::FlashHeld(uint64_t tag) const {
    std::lock_guard lock(m_mutex);
    const FrameRow* frame = Find(tag);
    return frame != nullptr && frame->flash;
}

void Recorder::SetCollectRows(bool collect) {
    std::lock_guard lock(m_mutex);
    m_collectRows = collect;
    if (!collect) {
        m_rows.clear();
    }
}

std::vector<FrameRow> Recorder::TakeCompletedRows() {
    std::lock_guard lock(m_mutex);
    std::vector<FrameRow> rows;
    rows.swap(m_rows);
    return rows;
}

uint64_t NowNanos() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

Recorder& Global() {
    static Recorder recorder;
    return recorder;
}

std::string CsvHeader() {
    return "frame,tick_start_ms,input_ms,seal_ms,first_present_ms,native_present_ms,"
           "input_to_first_ms,input_to_native_ms,tick_cpu_ms,flash";
}

std::string CsvRow(const FrameRow& row, uint64_t originNs) {
    const auto field = [](uint64_t from, uint64_t to) {
        if (from == 0 || to == 0) {
            return std::string{};
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%.3f", Milliseconds(from, to));
        return std::string{text};
    };
    std::string line = std::to_string(row.tag);
    for (const uint64_t time : {row.tickStartNs, row.inputNs, row.sealNs, row.firstPresentNs, row.nativePresentNs}) {
        line += ',' + field(originNs, time);
    }
    line += ',' + field(row.inputNs, row.firstPresentNs);
    line += ',' + field(row.inputNs, row.nativePresentNs);
    line += ',' + field(row.tickStartNs, row.sealNs);
    line += row.flash ? ",1" : ",0";
    return line;
}

} // namespace LatencyStats
