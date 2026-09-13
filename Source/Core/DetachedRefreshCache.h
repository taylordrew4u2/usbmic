#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mma {

/// A small snapshot cache for probes that can disappear into an unbounded OS
/// call (for example, asking a dead network/removable volume for its label).
///
/// getAndRefresh() only copies the last snapshot on the caller. A due refresh
/// runs on a detached worker which owns both its scan callable and this cache's
/// shared state. Destroying the cache therefore never waits for the OS and the
/// worker never refers back to its former owner. At most one refresh may be in
/// flight; if that refresh is permanently stuck, callers retain the last safe
/// snapshot rather than accumulating more stuck threads.
template <typename Snapshot>
class DetachedRefreshCache
{
public:
    using Clock = std::chrono::steady_clock;

    explicit DetachedRefreshCache (Snapshot initialSnapshot)
        : state (std::make_shared<State> (std::move (initialSnapshot)))
    {
    }

    DetachedRefreshCache (const DetachedRefreshCache&) = delete;
    DetachedRefreshCache& operator= (const DetachedRefreshCache&) = delete;

    /// Returns immediately with the current snapshot. When a refresh is due,
    /// starts it in the background. `scan` is moved into that detached worker;
    /// it must therefore own everything it uses rather than capture this
    /// cache's enclosing object.
    template <typename Scan>
    Snapshot getAndRefresh (std::chrono::milliseconds refreshInterval, Scan&& scan) const
    {
        const auto shared = state;
        const auto now = Clock::now();
        bool launch = false;
        Snapshot current;

        {
            const std::lock_guard<std::mutex> guard (shared->mutex);
            const auto interval = std::max (std::chrono::milliseconds::zero(),
                                            refreshInterval);
            const bool due = ! shared->hasFinishedRefresh
                          || now - shared->lastFinishedAt >= interval;

            if (due && ! shared->refreshRunning)
            {
                shared->refreshRunning = true;
                launch = true;
            }

            current = shared->snapshot;
        }

        if (! launch)
            return current;

        using ScanFunction = std::decay_t<Scan>;

        try
        {
            ScanFunction work (std::forward<Scan> (scan));
            std::thread ([shared, work = std::move (work)] () mutable
            {
                try
                {
                    auto fresh = work();
                    const std::lock_guard<std::mutex> guard (shared->mutex);
                    shared->snapshot = std::move (fresh);
                    shared->lastFinishedAt = Clock::now();
                    shared->hasFinishedRefresh = true;
                    shared->refreshRunning = false;
                }
                catch (...)
                {
                    // Keep the previous usable snapshot and rate-limit a
                    // retry just like a successful refresh. A probe failure
                    // must never escape a detached thread and terminate the
                    // process.
                    const std::lock_guard<std::mutex> guard (shared->mutex);
                    shared->lastFinishedAt = Clock::now();
                    shared->hasFinishedRefresh = true;
                    shared->refreshRunning = false;
                }
            }).detach();
        }
        catch (...)
        {
            // Thread construction can fail under resource pressure. Restore
            // the gate so a later, rate-limited call can try again.
            const std::lock_guard<std::mutex> guard (shared->mutex);
            shared->lastFinishedAt = Clock::now();
            shared->hasFinishedRefresh = true;
            shared->refreshRunning = false;
        }

        return current;
    }

private:
    struct State
    {
        explicit State (Snapshot initialSnapshot)
            : snapshot (std::move (initialSnapshot))
        {
        }

        std::mutex mutex;
        Snapshot snapshot;
        typename Clock::time_point lastFinishedAt {};
        bool hasFinishedRefresh = false;
        bool refreshRunning = false;
    };

    std::shared_ptr<State> state;
};

/// One background operation whose OS call may never return. The detached
/// worker owns its callable and shared state, so destroying this handle only
/// requests cancellation and never joins. Cancellation is cooperative: it
/// suppresses publication immediately even when the worker is still trapped in
/// a syscall. Only one operation can be in flight for a handle at a time.
template <typename Result>
class DetachedResultTask
{
public:
    struct PollResult
    {
        bool running = false;
        std::optional<Result> result;
    };

    DetachedResultTask() : state (std::make_shared<State>()) {}
    ~DetachedResultTask() { cancel(); }

    DetachedResultTask (const DetachedResultTask&) = delete;
    DetachedResultTask& operator= (const DetachedResultTask&) = delete;

    /// Work receives the cooperative cancellation flag. It must own everything
    /// else it uses; in particular it must not capture the object containing
    /// this task. Returns false when another operation is already in flight or
    /// when the worker could not be created.
    template <typename Work>
    bool start (Work&& work) const
    {
        const auto shared = state;

        {
            const std::lock_guard<std::mutex> guard (shared->mutex);
            if (shared->running)
                return false;

            shared->cancelled.store (false, std::memory_order_release);
            shared->result.reset();
            shared->running = true;
        }

        using WorkFunction = std::decay_t<Work>;

        try
        {
            WorkFunction ownedWork (std::forward<Work> (work));
            std::thread worker ([shared, ownedWork = std::move (ownedWork)] () mutable
            {
                std::optional<Result> completed;

                try
                {
                    completed.emplace (ownedWork (std::as_const (shared->cancelled)));
                }
                catch (...)
                {
                    // Detached exceptions must not terminate the process. The
                    // absence of a result leaves the caller's previous safe
                    // state in place.
                }

                const std::lock_guard<std::mutex> guard (shared->mutex);
                if (! shared->cancelled.load (std::memory_order_acquire)
                    && completed.has_value())
                    shared->result = std::move (completed);
                shared->running = false;
            });
            worker.detach();
        }
        catch (...)
        {
            const std::lock_guard<std::mutex> guard (shared->mutex);
            shared->running = false;
            return false;
        }

        return true;
    }

    std::optional<Result> takeResult() const
    {
        const std::lock_guard<std::mutex> guard (state->mutex);
        auto result = std::move (state->result);
        state->result.reset();
        return result;
    }

    /// Observes completion and consumes its result under one lock. Callers
    /// which decide whether an operation is still a safety gate must not read
    /// `takeResult()` and `isRunning()` separately: a worker can publish in
    /// between those reads, leaving the caller with the impossible-looking
    /// pair "no result, not running" and briefly authorizing unsafe work.
    PollResult poll() const
    {
        const std::lock_guard<std::mutex> guard (state->mutex);
        PollResult snapshot;
        snapshot.running = state->running;
        snapshot.result = std::move (state->result);
        state->result.reset();
        return snapshot;
    }

    bool isRunning() const
    {
        const std::lock_guard<std::mutex> guard (state->mutex);
        return state->running;
    }

    void cancel() const noexcept
    {
        state->cancelled.store (true, std::memory_order_release);
        const std::lock_guard<std::mutex> guard (state->mutex);
        state->result.reset();
    }

    /// Detaches this handle from a cancelled worker that may be permanently
    /// stuck. The old worker keeps its old state; subsequent starts use a fresh
    /// one, and a late old result has nowhere to publish into this handle.
    void abandon()
    {
        auto replacement = std::make_shared<State>();
        auto previous = std::exchange (state, std::move (replacement));
        previous->cancelled.store (true, std::memory_order_release);
        const std::lock_guard<std::mutex> guard (previous->mutex);
        previous->result.reset();
    }

private:
    struct State
    {
        std::mutex mutex;
        std::atomic<bool> cancelled { false };
        std::optional<Result> result;
        bool running = false;
    };

    std::shared_ptr<State> state;
};

/// Serializes detached filesystem mutations by inert path string. A worker may
/// be abandoned by its UI owner while the OS still has it trapped in a syscall;
/// the lease remains active until that worker really returns, so another worker
/// (or a recording gate) can refuse to touch the same root in the meantime.
///
/// The shared state is owned by leases as well as this handle. Destroying the
/// handle therefore never waits for an old worker and never leaves the worker
/// referring to freed bookkeeping.
class DetachedPathMutationGate
{
private:
    struct State;

    /// Normalize a path without asking the filesystem anything. Recovery and
    /// preflight use this gate specifically because a stale removable volume
    /// may block inside stat/realpath, so canonicalization must stay purely
    /// lexical here.
    static std::string normalizePath (std::string path)
    {
        std::replace (path.begin(), path.end(), '\\', '/');

        std::string prefix;
        size_t cursor = 0;

        if (path.size() >= 2
            && std::isalpha (static_cast<unsigned char> (path[0]))
            && path[1] == ':')
        {
            prefix.push_back (static_cast<char> (
                std::tolower (static_cast<unsigned char> (path[0]))));
            prefix.push_back (':');
            cursor = 2;

            if (cursor < path.size() && path[cursor] == '/')
            {
                prefix.push_back ('/');
                while (cursor < path.size() && path[cursor] == '/')
                    ++cursor;
            }
        }
        else if (path.rfind ("//", 0) == 0)
        {
            prefix = "//";
            cursor = 2;
            while (cursor < path.size() && path[cursor] == '/')
                ++cursor;
        }
        else if (! path.empty() && path[0] == '/')
        {
            prefix = "/";
            cursor = 1;
            while (cursor < path.size() && path[cursor] == '/')
                ++cursor;
        }

        std::vector<std::string> components;
        while (cursor <= path.size())
        {
            const auto separator = path.find ('/', cursor);
            const auto end = separator == std::string::npos ? path.size() : separator;
            auto component = path.substr (cursor, end - cursor);

            if (! component.empty() && component != ".")
            {
                if (component == "..")
                {
                    if (! components.empty() && components.back() != "..")
                        components.pop_back();
                    else if (prefix.empty())
                        components.push_back (std::move (component));
                }
                else
                {
                   #if defined(_WIN32) || defined(__APPLE__)
                    // The ordinary filesystems on both supported desktop
                    // targets are case-insensitive. Treat a case-sensitive
                    // volume conservatively too: serializing two paths which
                    // happen to be distinct is safe; racing one path under two
                    // spellings is not.
                    std::transform (component.begin(), component.end(), component.begin(),
                                    [] (unsigned char c)
                                    {
                                        return static_cast<char> (std::tolower (c));
                                    });
                   #endif
                    components.push_back (std::move (component));
                }
            }

            if (separator == std::string::npos)
                break;

            cursor = separator + 1;
            while (cursor < path.size() && path[cursor] == '/')
                ++cursor;
        }

        std::string normalized = prefix;
        for (const auto& component : components)
        {
            if (! normalized.empty() && normalized.back() != '/')
                normalized.push_back ('/');
            normalized += component;
        }

        return normalized;
    }

    static bool isSameOrDescendant (const std::string& path,
                                    const std::string& possibleAncestor)
    {
        if (path == possibleAncestor)
            return true;

        if (possibleAncestor.empty()
            || path.size() <= possibleAncestor.size()
            || path.compare (0, possibleAncestor.size(), possibleAncestor) != 0)
            return false;

        return possibleAncestor.back() == '/' || path[possibleAncestor.size()] == '/';
    }

    static bool pathsOverlap (const std::string& first, const std::string& second)
    {
        return isSameOrDescendant (first, second)
            || isSameOrDescendant (second, first);
    }

public:
    class Lease;
    using LeasePtr = std::shared_ptr<Lease>;

    DetachedPathMutationGate() : state (std::make_shared<State>()) {}

    // A detached worker may safely own a handle to the same gate. This is what
    // lets it resolve symlinks/aliases away from the UI thread before taking a
    // lease, without ever capturing the Application which created the gate.
    DetachedPathMutationGate (const DetachedPathMutationGate&) noexcept = default;
    DetachedPathMutationGate& operator= (const DetachedPathMutationGate&) noexcept = default;

    /// Acquires every non-empty path atomically. Returns empty when any of them
    /// is still owned by an earlier detached mutation.
    LeasePtr tryAcquire (std::vector<std::string> paths) const
    {
        for (auto& path : paths)
            path = normalizePath (std::move (path));

        paths.erase (std::remove_if (paths.begin(), paths.end(), [] (const auto& path)
        {
            return path.empty();
        }), paths.end());
        std::sort (paths.begin(), paths.end());
        paths.erase (std::unique (paths.begin(), paths.end()), paths.end());

        if (paths.empty())
            return {};

        auto lease = std::make_shared<Lease> (state, std::move (paths));

        {
            const std::lock_guard<std::mutex> guard (state->mutex);
            for (const auto& path : lease->paths)
                if (std::any_of (state->activePaths.begin(), state->activePaths.end(),
                                 [&path] (const auto& active)
                                 {
                                     return pathsOverlap (path, active);
                                 }))
                    return {};

            for (const auto& path : lease->paths)
                state->activePaths.push_back (path);

            lease->armed = true;
        }

        return lease;
    }

    bool isActive (const std::string& path) const
    {
        if (path.empty())
            return false;

        const auto normalized = normalizePath (path);
        const std::lock_guard<std::mutex> guard (state->mutex);
        return std::any_of (state->activePaths.begin(), state->activePaths.end(),
                            [&normalized] (const auto& active)
                            {
                                return pathsOverlap (normalized, active);
                            });
    }

private:
    struct State
    {
        std::mutex mutex;
        std::vector<std::string> activePaths;
    };

public:
    class Lease
    {
    public:
        Lease (std::shared_ptr<State> sharedState, std::vector<std::string> ownedPaths)
            : state (std::move (sharedState)), paths (std::move (ownedPaths))
        {
        }

        ~Lease()
        {
            if (! armed)
                return;

            const std::lock_guard<std::mutex> guard (state->mutex);
            for (const auto& path : paths)
            {
                const auto found = std::find (state->activePaths.begin(),
                                              state->activePaths.end(), path);
                if (found != state->activePaths.end())
                    state->activePaths.erase (found);
            }
        }

        Lease (const Lease&) = delete;
        Lease& operator= (const Lease&) = delete;

    private:
        std::shared_ptr<State> state;
        std::vector<std::string> paths;
        bool armed = false;

        friend class DetachedPathMutationGate;
    };

private:
    std::shared_ptr<State> state;
};

} // namespace mma
