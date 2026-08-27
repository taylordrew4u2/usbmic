#pragma once
#include <vector>

namespace mma {

struct BufferSizeChange
{
    double atSeconds = 0.0;
    int fromSamples = 0;
    int toSamples = 0;
};

/// §5.4 buffer ladder. Starts at 64 samples and steps up only when the callback
/// is genuinely failing, because every step costs latency against the 10 ms
/// ceiling. It never steps down mid-recording: a buffer change during a take is
/// a dropout risk, and §0.1 puts not losing audio above everything else.
class BufferLadder
{
public:
    static constexpr int kSizes[] = { 64, 128, 256, 512 };
    static constexpr int kNumSizes = 4;

    /// Three or more overruns inside any 30-second window is the trigger.
    static constexpr int kOverrunTrigger = 3;
    static constexpr double kWindowSeconds = 30.0;

    int getCurrentSize() const noexcept { return kSizes[currentIndex]; }
    bool isAtMaximum() const noexcept { return currentIndex == kNumSizes - 1; }

    void setRecording (bool recording) noexcept { isRecording = recording; }

    /// Records a callback overrun at the given time. Returns true when this
    /// overrun triggered a step up, so the caller can log it (§5.4) and tell
    /// the user in plain language.
    bool noteOverrun (double nowSeconds);

    /// §5.4: re-evaluate only on next launch or on device change, never
    /// automatically during a recording. Refuses while recording.
    bool resetToLowest() noexcept;

    const std::vector<BufferSizeChange>& getChangeLog() const noexcept { return changeLog; }

private:
    int currentIndex = 0;
    bool isRecording = false;
    std::vector<double> recentOverruns;
    std::vector<BufferSizeChange> changeLog;

    void dropOverrunsBefore (double cutoffSeconds);
};

} // namespace mma
