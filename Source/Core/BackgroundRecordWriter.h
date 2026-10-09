#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
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
///
/// Stop is the boundary, and no refresh may be put in place after the
/// stop-time record: that would put "no stop time" back over a finished take.
/// Every text this writer is given is numbered, and is put in place only
/// through the commit it hands the write function -- the rename, run under a
/// lock every write of this file shares, and refused when the take has
/// stopped (for a refresh) or a newer text is already in place. So a refresh
/// stuck in a slow drive for any length of time cannot land on top of the
/// stop-time record, whichever of them the drive lets finish first.
///
/// retire() is that boundary for a caller writing the stop-time record by
/// other means: after it returns true no refresh can land. writeLast() is that
/// boundary with the stop-time record written here, ordered after anything in
/// flight however long the drive takes, and waited for no longer than the
/// caller allows. A worker stuck on a dead drive owns only the shared state,
/// never its owner.
class BackgroundRecordWriter
{
public:
    /// Puts a finished temporary copy in place over the file: the rename.
    using Rename = std::function<bool()>;

    /// What the write function calls with its rename once the temporary copy
    /// holds every byte. The rename runs only if this text may still be put in
    /// place, and only one rename of this file runs at a time; false means it
    /// did not run (or failed), and the temporary copy is the write function's
    /// to remove.
    using Commit = std::function<bool (const Rename&)>;

    /// Writes `text` to `path`, all of it or nothing, putting it in place only
    /// through `commit` (the caller supplies the checked, temp-then-rename
    /// write). The result of a refresh is not needed: the next refresh, or the
    /// stop-time write, tries again.
    using WriteFunction = std::function<bool (const std::string& path, const std::string& text,
                                              const Commit& commit)>;

    explicit BackgroundRecordWriter (WriteFunction write)
        : state (std::make_shared<State>())
    {
        state->write = std::move (write);
    }

    /// Never waits. A refresh not yet started is dropped, and one still being
    /// written is refused at its rename; one already inside its rename
    /// finishes, or does not, on its own worker. A record handed to
    /// writeLast() is still put in place.
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
            s->pendingNumber = ++s->issued;

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

    /// No refresh is put in place after this returns true. False means a
    /// refresh was still inside its rename when `waitAtMost` ran out: that
    /// drive is not answering, and that refresh may still land later. A
    /// refresh still writing its temporary copy does not hold this up: it is
    /// refused when it reaches its rename.
    bool retire (std::chrono::milliseconds waitAtMost)
    {
        std::unique_lock<std::mutex> guard (state->mutex);
        state->retired = true;
        state->pending.reset();

        // A refresh that passed its check before the flag was set is counted
        // in renamesInFlight under this same mutex, so it is either counted
        // here or it sees the flag and never renames.
        return state->renameFinished.wait_for (guard, waitAtMost,
                                               [this] { return state->renamesInFlight == 0; });
    }

    /// The stop-time record: retires the refreshes and writes `text` on a
    /// worker of its own, put in place after any refresh still in flight --
    /// one that has not reached its rename yet never will, and one inside its
    /// rename now is waited for on that worker, not here. Waits at most
    /// `waitAtMost` for the answer; empty means it did not come in time, and
    /// the record is still written, and still last, whenever the drive lets
    /// it be.
    std::optional<bool> writeLast (std::string path, std::string text, std::chrono::milliseconds waitAtMost)
    {
        auto s = state;
        std::uint64_t number = 0;
        {
            const std::lock_guard<std::mutex> guard (s->mutex);
            s->retired = true;
            s->pending.reset();
            number = ++s->issued;
        }

        auto outcome = std::make_shared<std::promise<bool>>();
        auto answer = outcome->get_future();

        try
        {
            std::thread ([s, number, outcome, path = std::move (path), text = std::move (text)]
            {
                bool written = false;

                try
                {
                    written = s->write (path, text, commitFor (s, number, true));
                }
                catch (...)
                {
                }

                outcome->set_value (written);
            }).detach();
        }
        catch (...)
        {
            return false;
        }

        if (answer.wait_for (waitAtMost) != std::future_status::ready)
            return std::nullopt;

        return answer.get();
    }

    /// For tests: true while a refresh worker is between taking a text and
    /// finishing.
    bool isBusy() const
    {
        const std::lock_guard<std::mutex> guard (state->mutex);
        return state->busy;
    }

private:
    struct State
    {
        mutable std::mutex mutex;          // the fields below; never held across I/O
        std::mutex renameOrder;            // one rename at a time, held across a rename only
        std::condition_variable renameFinished; // signalled, under `mutex`, as a rename ends
        int renamesInFlight = 0;           // renames that passed the guard and have not ended
        WriteFunction write;
        std::optional<std::pair<std::string, std::string>> pending;
        std::uint64_t pendingNumber = 0;
        std::uint64_t issued = 0;          // the number given to the newest text
        std::uint64_t inPlace = 0;         // the number of the newest text put in place
        bool busy = false;
        bool retired = false;
    };

    // The generation guard, checked under the rename lock right before the
    // rename: a refresh after Stop, or any text older than the one already in
    // place, is refused. The guard and retire()'s flag share one mutex, so
    // either retire() counts a rename already past the guard and waits for it,
    // or that refresh sees the flag and never renames.
    static Commit commitFor (std::shared_ptr<State> s, std::uint64_t number, bool isLast)
    {
        return [s = std::move (s), number, isLast] (const Rename& rename)
        {
            // A plain mutex and a counter, not a timed mutex: retire() waits on
            // the counter with a deadline instead of trying the lock, which
            // ThreadSanitizer cannot follow through glibc's clocked lock.
            const std::lock_guard<std::mutex> order (s->renameOrder);
            {
                const std::lock_guard<std::mutex> guard (s->mutex);

                if (number <= s->inPlace || (s->retired && ! isLast))
                    return false;

                ++s->renamesInFlight;
            }

            bool renamed = false;

            try
            {
                renamed = rename();
            }
            catch (...)
            {
            }

            {
                const std::lock_guard<std::mutex> guard (s->mutex);

                if (renamed)
                    s->inPlace = number;

                --s->renamesInFlight;
            }

            s->renameFinished.notify_all();
            return renamed;
        };
    }

    static void run (std::shared_ptr<State> s)
    {
        for (;;)
        {
            std::pair<std::string, std::string> job;
            std::uint64_t number = 0;
            {
                const std::lock_guard<std::mutex> guard (s->mutex);

                if (! s->pending.has_value() || s->retired)
                {
                    s->busy = false;
                    return;
                }

                job = std::move (*s->pending);
                number = s->pendingNumber;
                s->pending.reset();
            }

            try
            {
                (void) s->write (job.first, job.second, commitFor (s, number, false));
            }
            catch (...)
            {
            }
        }
    }

    std::shared_ptr<State> state;
};

} // namespace mma
