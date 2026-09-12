#include "FilesystemStatusProbe.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace mma {

namespace fs = std::filesystem;

FilesystemStatusProbe::FilesystemStatusProbe (std::chrono::milliseconds refreshInterval)
    : interval (std::max (std::chrono::milliseconds (1), refreshInterval))
{
    // Start only after every member has completed construction. Launching the
    // thread from the member-initializer list let run() observe `stopping` and
    // `hasRequest` while those later-declared fields were still being
    // initialized; ThreadSanitizer caught that constructor-order race.
    worker = std::thread ([this] { run(); });
}

FilesystemStatusProbe::~FilesystemStatusProbe()
{
    stop();
}

void FilesystemStatusProbe::setRequest (Request request)
{
    {
        const std::lock_guard<std::mutex> guard (mutex);
        if (stopping || (hasRequest && request == requested))
            return;

        requested = std::move (request);
        hasRequest = true;
        ++requestGeneration;
    }

    condition.notify_all();
}

FilesystemStatusProbe::Snapshot FilesystemStatusProbe::getSnapshot() const
{
    const std::lock_guard<std::mutex> guard (mutex);
    return latest;
}

bool FilesystemStatusProbe::waitForRevisionAfter (uint64_t revision, Snapshot& result,
                                                   std::chrono::milliseconds timeout) const
{
    std::unique_lock<std::mutex> lock (mutex);
    const bool changed = condition.wait_for (lock, timeout, [this, revision]
    {
        return stopping || latest.revision > revision;
    });

    result = latest;
    return changed && result.revision > revision;
}

void FilesystemStatusProbe::stop()
{
    {
        const std::lock_guard<std::mutex> guard (mutex);
        if (stopping)
            return;
        stopping = true;
    }

    condition.notify_all();
    if (worker.joinable())
        worker.join();
}

FilesystemStatusProbe::Snapshot FilesystemStatusProbe::sample (const Request& request)
{
    Snapshot out;
    out.request = request;
    out.ready = true;
    out.sampledOnThread = std::this_thread::get_id();

    if (! request.destinationPath.empty() && request.bytesPerSecond > 0.0)
    {
        std::error_code error;
        const fs::path destination (request.destinationPath);
        if (fs::is_directory (destination, error) && ! error)
        {
            const auto space = fs::space (destination, error);
            if (! error)
                out.remainingSeconds = static_cast<double> (space.available)
                                     / request.bytesPerSecond;
        }
    }

    if (! request.mirrorPath.empty())
    {
        std::error_code error;
        const fs::path mirror (request.mirrorPath);
        if (fs::is_directory (mirror, error) && ! error)
        {
            const auto space = fs::space (mirror, error);
            if (! error && space.available <= static_cast<uintmax_t> (INT64_MAX))
                out.mirrorFreeBytes = static_cast<int64_t> (space.available);
        }
    }

    if (! request.sessionFolder.empty())
    {
        std::error_code error;
        const fs::path folder (request.sessionFolder);
        const bool isDirectory = fs::is_directory (folder, error);

        if (! error && isDirectory)
        {
            fs::directory_iterator iterator (folder, error), end;
            while (! error && iterator != end)
            {
                const auto& entry = *iterator;
                std::error_code entryError;
                const bool isRegularFile = entry.is_regular_file (entryError);
                if (entryError)
                {
                    error = entryError;
                    break;
                }

                if (isRegularFile)
                {
                    const auto size = entry.file_size (entryError);
                    if (entryError)
                    {
                        error = entryError;
                        break;
                    }

                    if (size <= static_cast<uintmax_t> (INT64_MAX))
                        out.files.push_back ({ entry.path().filename().string(),
                                               static_cast<int64_t> (size) });
                }

                iterator.increment (error);
            }
        }

        // A missing folder is a valid empty observation; a system-call error
        // is not. Do not expose a partial listing as zero/growth evidence.
        out.filesObservationReady = ! error;
        if (! out.filesObservationReady)
            out.files.clear();

        const auto rank = [] (const std::string& name)
        {
            std::string lower = name;
            std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c)
            { return static_cast<char> (std::tolower (c)); });
            if (lower.rfind ("mix", 0) == 0) return 0;
            if (lower == "session.json") return 2;
            return 1;
        };

        std::sort (out.files.begin(), out.files.end(), [&rank] (const File& a, const File& b)
        {
            const int ar = rank (a.name), br = rank (b.name);
            return ar != br ? ar < br : a.name < b.name;
        });
    }

    return out;
}

void FilesystemStatusProbe::run()
{
    std::unique_lock<std::mutex> lock (mutex);
    condition.wait (lock, [this] { return stopping || hasRequest; });

    while (! stopping)
    {
        const auto request = requested;
        const auto generation = requestGeneration;

        lock.unlock();
        auto snapshot = sample (request);
        lock.lock();

        // Do not publish a slow result for a destination that was replaced
        // while the card was being queried.
        if (generation == requestGeneration)
        {
            snapshot.revision = latest.revision + 1;
            latest = std::move (snapshot);
            condition.notify_all();
        }

        condition.wait_for (lock, interval, [this, generation]
        {
            return stopping || generation != requestGeneration;
        });
    }
}

} // namespace mma
