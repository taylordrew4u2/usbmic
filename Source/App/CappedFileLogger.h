#pragma once
#include <juce_core/juce_core.h>
#include <cstring>
#include "../Core/LogSizeBudget.h"

namespace mma {

/// juce::FileLogger, kept under its cap for the whole session.
///
/// FileLogger trims only when it is constructed, so the cap held at launch and
/// nowhere after: an app left running for days grew its log without limit
/// until it was next started. This wraps one and trims the file again whenever
/// LogSizeBudget says the lines written since have pushed it past the cap.
///
/// The check is arithmetic on a running total; the file is only read when a
/// trim is due, at most once per quarter-cap of logging. Lines come from the
/// message thread and worker threads -- never the audio callback, which does
/// not log -- and one lock serialises the append and the trim, so a trim never
/// races a write.
class CappedFileLogger : public juce::Logger
{
public:
    CappedFileLogger (const juce::File& file, const juce::String& welcomeMessage, int maxBytes)
        : inner (file, welcomeMessage, maxBytes),
          budget (maxBytes, file.getSize())
    {
    }

    void logMessage (const juce::String& message) override
    {
        const juce::ScopedLock sl (lock);

        inner.logMessage (message);

        // FileLogger writes the line plus JUCE's newLine ("\r\n").
        const auto written = static_cast<std::int64_t> (message.getNumBytesAsUTF8())
                           + static_cast<std::int64_t> (std::strlen (juce::NewLine::getDefault()));

        if (budget.noteWritten (written))
        {
            juce::FileLogger::trimFileSize (inner.getLogFile(), budget.trimTarget());
            budget.resync (inner.getLogFile().getSize());
        }
    }

    const juce::File& getLogFile() const noexcept { return inner.getLogFile(); }

private:
    juce::FileLogger inner;
    LogSizeBudget budget;
    juce::CriticalSection lock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CappedFileLogger)
};

} // namespace mma
