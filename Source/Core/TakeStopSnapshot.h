#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace mma {

/// The figures session.json's stop-time rewrite reports about a take.
struct TakeFigures
{
    /// Seconds from the take's audio t=0. Every stop-time drift and dropout
    /// entry is stamped with it.
    double elapsedSeconds = 0.0;
    std::string stopTimestampIso;
    int bufferSizeSamples = 0;

    uint64_t framesDropped = 0;
    uint64_t overrunSamples = 0;
    uint64_t underrunSamples = 0;
    uint64_t framesMissedByLayout = 0;
    uint64_t backendFramesDropped = 0;
};

/// What the take looked like at the moment Stop was pressed. Captured then,
/// cleared when the next take starts.
class TakeStopSnapshot
{
public:
    void capture (const TakeFigures& atStop) { snapshot = atStop; }
    void clear() { snapshot.reset(); }
    bool hasSnapshot() const noexcept { return snapshot.has_value(); }

    /// The figures a session.json write should use: the snapshot for the
    /// stop-time rewrite, the live state for the write at the take's start.
    ///
    /// The stop-time write used to read live state too, which was right only
    /// while it ran inside the Stop press. A camera still finishing its movie
    /// defers it until that completes; by then the engine has stopped, so the
    /// elapsed time read zero and every drift and dropout entry was stamped
    /// 0.0 s, and the counters had run on through monitoring since.
    TakeFigures resolve (bool sessionHasStopped, const TakeFigures& live) const
    {
        return sessionHasStopped && snapshot.has_value() ? *snapshot : live;
    }

private:
    std::optional<TakeFigures> snapshot;
};

} // namespace mma
