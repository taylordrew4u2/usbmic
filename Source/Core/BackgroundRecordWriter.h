#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace mma {

/// Keeps one small file (a take's session.json) up to date during a take
/// without the caller ever waiting on the drive it lives on.
///
/// session.json was written at the start of a take and again at Stop, and
/// nowhere in between -- so after a crash, a force-quit or a power cut the
/// record beside the recovered audio said nothing about the microphone pulled
/// twenty minutes in, the camera that joined, or the backup that stopped.
/// Refreshing it means writing to the card mid-take, and a card that has
/// stopped answering never returns from that call; the message thread must not
/// be the one to make it.
///
/// submit() only stores the newest text and, if no worker is busy, starts a
/// detached one; the worker writes the latest text it finds until none is
/// pending, so a slow drive coalesces refreshes instead of queueing them.
/// retire() is the Stop boundary: after it returns true no refresh can land,
/// so the stop-time record written next can never be overwritten by an older
/// one that was still in flight. A worker stuck on a dead drive owns only the
/// shared state, never its owner.
class BackgroundRecordWriter
{
public:
    /// Writes `text` to `path`, all of it or nothing (the caller supplies the
    /// checked, temp-then-rename write). The result is not needed: the next
    /// refresh, or the stop-time write, tries again.
    using WriteFunction = std::function<bool (const std::string& path, const std::string& text)>;

    explicit BackgroundRecordWriter (WriteFunction write)
        : state (std::make_shared<State>())
    {
        state->write = std::move (write);
    }

    /// Never waits. A refresh not yet started is dropped; one already inside
    /// the drive finishes, or does not, on its own worker.
    ~BackgroundRecordWriter()
    {
        const std::lock_guard<std::mutex> guard (state->mutex);
        state->retired = true;
        state->pending.reset();
    }

    BackgroundRecordWriter (const BackgroundRecordWriter&) = delete;
    BackgroundRecordWriter& operator= (const BackgroundRecordWriter&) = delete;

    /// Never blocks on the drive. Ignored once retired.
    void submit (std::string path, std::string text)
    {
        auto s = state;
        {
            const std::lock_guard<std::mutex> guard (s->mutex);

            if (s->retired)
                return;

            s->pending = std::make_pair (std::move (path), std::move (text));

            if (s->busy)
                return;

            s->busy = true;
        }

        try
        {
            std::thread ([s] { run (s); }).detach();
        }
        catch (...)
        {
            const std::lock_guard<std::mutex> guard (s->mutex);
            s->busy = false;
        }
    }

    /// No refresh is written after this returns true. False means a write was
    /// still inside the drive when `waitAtMost` ran out: that drive is not
    /// answering, and that write may still land later.
    bool retire (std::chrono::milliseconds waitAtMost)
    {
        {
            const std::lock_guard<std::mutex> guard (state->mutex);
            state->retired = true;
            state->pending.reset();
        }

        if (! state->io.try_lock_for (waitAtMost))
            return false;

        state->io.unlock();
        return true;
    }

    /// For tests: true while a worker is between taking a text and finishing.
    bool isBusy() const
    {
        const std::lock_guard<std::mutex> guard (state->mutex);
        return state->busy;
    }

private:
    struct State
    {
        mutable std::mutex mutex;          // the fields below; never held across I/O
        std::timed_mutex io;               // held across the write itself
        WriteFunction write;
        std::optional<std::pair<std::string, std::string>> pending;
        bool busy = false;
        bool retired = false;
    };

    static void run (std::shared_ptr<State> s)
    {
        for (;;)
        {
            std::pair<std::string, std::string> job;
            {
                const std::lock_guard<std::mutex> guard (s->mutex);

                if (! s->pending.has_value() || s->retired)
                {
                    s->busy = false;
                    return;
                }

                job = std::move (*s->pending);
                s->pending.reset();
            }

            // The io lock first, the retired check second: retire() sets the
            // flag before it asks for this lock, so either it sees this write
            // finish or this write sees the flag and never starts.
            const std::lock_guard<std::timed_mutex> io (s->io);
            {
                const std::lock_guard<std::mutex> guard (s->mutex);

                if (s->retired)
                {
                    s->busy = false;
                    return;
                }
            }

            try
            {
                (void) s->write (job.first, job.second);
            }
            catch (...)
            {
            }
        }
    }

    std::shared_ptr<State> state;
};

} // namespace mma
