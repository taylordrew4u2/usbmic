#pragma once
#include <juce_events/juce_events.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace mma {

/// Test builds only. Measures how long the message thread -- the thread that
/// draws the window and answers every click -- goes without running, so a gate
/// can prove the window stayed responsive through a hostile event instead of
/// inferring it from a log line written after the fact.
///
/// A timer on the message thread stamps a heartbeat every 20 ms. A watcher
/// thread appends "STALL <ms>" to MMA_STALL_METER_FILE whenever the heartbeat
/// was missing for more than 250 ms, and keeps "<file>.now" holding how long it
/// has been missing right now, so a window that froze and never came back is
/// reported too. Inert unless that variable is set.
class MessageThreadStallMeter : private juce::Timer
{
public:
    MessageThreadStallMeter()
    {
        const char* path = std::getenv ("MMA_STALL_METER_FILE");
        if (path == nullptr || *path == '\0')
            return;

        file = path;
        beat();
        startTimer (20);
        watcher = std::thread ([this] { watch(); });
    }

    ~MessageThreadStallMeter() override
    {
        stopTimer();
        running.store (false);
        if (watcher.joinable())
            watcher.join();
    }

private:
    using Clock = std::chrono::steady_clock;

    static long long nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds> (
                   Clock::now().time_since_epoch()).count();
    }

    void beat() { lastBeatMs.store (nowMs()); }
    void timerCallback() override { beat(); }

    void watch()
    {
        long long worst = 0;

        while (running.load())
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));

            const auto gap = nowMs() - lastBeatMs.load();

            if (gap > kThresholdMs)
            {
                worst = gap > worst ? gap : worst;
            }
            else if (worst > 0)
            {
                std::ofstream (file, std::ios::app) << "STALL " << worst << "\n";
                worst = 0;
            }

            std::ofstream (file + ".now", std::ios::trunc) << gap << "\n";
        }
    }

    static constexpr long long kThresholdMs = 250;

    std::string file;
    std::atomic<long long> lastBeatMs { 0 };
    std::atomic<bool> running { true };
    std::thread watcher;
};

} // namespace mma
