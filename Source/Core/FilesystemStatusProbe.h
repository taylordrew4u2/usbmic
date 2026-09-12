#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mma {

/// The filesystem facts the UI needs while a take is live. Sampling them can
/// block on a slow or disappearing card, so this object owns the only worker
/// allowed to ask. Callers submit cheap in-memory requests and read snapshots.
class FilesystemStatusProbe
{
public:
    struct Request
    {
        std::string destinationPath;
        std::string sessionFolder;
        std::string mirrorPath;
        double bytesPerSecond = 0.0;

        bool operator== (const Request& other) const noexcept
        {
            return destinationPath == other.destinationPath
                && sessionFolder == other.sessionFolder
                && mirrorPath == other.mirrorPath
                && bytesPerSecond == other.bytesPerSecond;
        }
        bool operator!= (const Request& other) const noexcept { return ! (*this == other); }
    };

    struct File
    {
        std::string name;
        int64_t sizeBytes = 0;
    };

    struct Snapshot
    {
        Request request;
        bool ready = false;
        /// True only when the requested session folder was inspected without a
        /// filesystem error. A completed worker cycle can still lack this
        /// evidence (for example, while a removable card is disappearing).
        bool filesObservationReady = false;
        double remainingSeconds = -1.0;
        int64_t mirrorFreeBytes = -1;
        std::vector<File> files;
        uint64_t revision = 0;

        // Diagnostic/test evidence that blocking filesystem calls did not run
        // on the caller that submitted the request.
        std::thread::id sampledOnThread;
    };

    explicit FilesystemStatusProbe (
        std::chrono::milliseconds refreshInterval = std::chrono::milliseconds (500));
    ~FilesystemStatusProbe();

    /// Changes what the worker samples. Repeating an identical request is a
    /// no-op; the worker refreshes the active request at its own bounded rate.
    void setRequest (Request request);
    Snapshot getSnapshot() const;

    /// Deterministic test/support hook. Shipping UI code never waits.
    bool waitForRevisionAfter (uint64_t revision, Snapshot& result,
                               std::chrono::milliseconds timeout) const;

    void stop();

private:
    static Snapshot sample (const Request& request);
    void run();

    const std::chrono::milliseconds interval;
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    std::thread worker;
    bool stopping = false;
    bool hasRequest = false;
    uint64_t requestGeneration = 0;
    Request requested;
    Snapshot latest;
};

} // namespace mma
