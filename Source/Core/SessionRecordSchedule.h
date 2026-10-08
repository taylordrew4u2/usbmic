#pragma once

#include <string>
#include <utility>

namespace mma {

/// When a running take's session.json is rewritten.
///
/// It was written at Record and at Stop only, so a crash, a force-quit or a
/// power cut left a record of the take's first instant: none of the
/// microphones that dropped out, the cameras that joined, the backup that
/// stopped or the buffer that stepped up. Rewritten on a timer alone, the
/// record could still be missing the one event that explains the take -- the
/// unplug a few seconds before the crash. So both: soon after anything worth
/// recording changes, and every kPeriodSeconds regardless, which carries the
/// figures that move all the time (the take's length, drift, drop counts)
/// without rewriting the file for each of them.
///
/// "Anything worth recording" is a signature the caller builds from the
/// facts the record holds -- how many dropouts, how many movies, whether the
/// backup is still being written. Pure bookkeeping, no clock and no I/O, so
/// it can be checked without a take.
class SessionRecordSchedule
{
public:
    static constexpr double kPeriodSeconds = 30.0;

    /// The record was just written, describing the take as `signature` says.
    void written (double nowSeconds, std::string signature)
    {
        lastWrittenSeconds = nowSeconds;
        lastSignature = std::move (signature);
        started = true;
    }

    /// True when the record on disk no longer describes the take: something
    /// it counts has changed since the last write, or a period has passed.
    /// Never true before the first write -- that is the take's start, not a
    /// refresh.
    bool isDue (double nowSeconds, const std::string& signature) const
    {
        if (! started)
            return false;

        return signature != lastSignature
            || nowSeconds - lastWrittenSeconds >= kPeriodSeconds;
    }

    /// The take is over; nothing is due until the next one starts.
    void stop() { started = false; }

private:
    double lastWrittenSeconds = 0.0;
    std::string lastSignature;
    bool started = false;
};

} // namespace mma
