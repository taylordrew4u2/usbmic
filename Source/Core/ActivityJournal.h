#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace mma {

/// What a journal entry is telling the user. Ordered least to most serious --
/// getMostSeriousUnseen() compares these directly, so the order is load-bearing.
enum class ActivityLevel
{
    Started,   ///< something began: monitoring came up, a take started, a mic joined.
    Stopped,   ///< something ended on purpose: the take was stopped, monitoring was torn down.
    Recovered, ///< something that had failed is working again: a mic came back, monitoring resumed.
    Warning,   ///< still working, but degraded or heading for trouble.
    Failed,    ///< something stopped working and did not come back on its own.
};

struct ActivityEntry
{
    /// Identifies this entry for the life of the journal, so a caller that has
    /// shown one entry can mark exactly that one as seen. Marking everything
    /// seen because one thing was shown is how a second, quieter failure gets
    /// lost -- which is the failure mode this class exists to remove.
    uint64_t id = 0;

    /// Seconds on the app's own clock, from the same source the take's elapsed
    /// time uses, so an entry can be lined up against the recording.
    double atSeconds = 0.0;

    ActivityLevel level = ActivityLevel::Started;

    /// What this is about, in the user's words: "Recording", "Local backup",
    /// "Kitchen". Never a class name and never a device id -- §10.6.
    std::string subject;

    /// §10.6: what happened, then what to do, in one sentence. No codes.
    std::string message;

    /// How many times this same thing has happened in a row. 1 for a first
    /// occurrence; a repeat collapses into the existing entry and increments
    /// this rather than pushing a duplicate line the user has to scroll past.
    int repeats = 1;

    /// Cleared once the user has been shown this entry. Warnings and failures
    /// stay unseen until something acknowledges them; see getMostSeriousUnseen.
    bool seen = false;
};

/// The record of everything the app did and everything that went wrong, kept so
/// that nothing the user should know about can happen unannounced.
///
/// The app already had a channel for telling the user things: pollStatusAdvice
/// returns the single most serious sentence for the advice line. That channel is
/// necessary and it is not sufficient, for a reason that is structural rather
/// than cosmetic: it holds exactly one message, chosen by priority. Anything
/// that happens while a more serious message is on the line is never shown at
/// all -- not shown late, not shown smaller, never. A mic that dropped and came
/// back during a disk warning left no trace anywhere the user could look, which
/// is the same silence §6.5 spends a whole table forbidding.
///
/// So this is the durable half of the pair. Every start, every stop, every
/// failure and every recovery is written here as it happens; the advice line
/// stays the loud channel for the one thing that matters most right now, and
/// this is what the user can look back at afterwards -- and what gets written
/// into the take's own folder, so a take carries the story of how it went.
///
/// Bounded, because it runs for the length of a session and must never be the
/// reason a long take runs out of memory. When the bound is reached the oldest
/// entries go first, except that an unseen failure is never dropped to make
/// room for something less serious (see note()).
class ActivityJournal
{
public:
    /// How many entries are kept. Roughly an hour of a rig behaving badly.
    static constexpr size_t kMaxEntries = 256;

    /// A repeat of the same subject and message within this many seconds
    /// collapses into the existing entry instead of adding a line.
    static constexpr double kRepeatWindowSeconds = 20.0;

    /// Records one thing that happened. Safe to call from any thread that is
    /// not the audio callback (it allocates and takes a lock, so it is not
    /// safe from one, and nothing on the audio path should be calling it --
    /// the audio thread raises a flag and the poll turns it into an entry).
    void note (double nowSeconds, ActivityLevel level, std::string subject, std::string message);

    /// Newest first. A copy, so a UI can walk it without holding the lock.
    std::vector<ActivityEntry> getEntries() const;

    /// The most serious thing the user has not been shown yet, or false when
    /// there is nothing. Ties break toward the newest, so an ongoing failure
    /// reports its current form rather than its first.
    bool getMostSeriousUnseen (ActivityEntry& out) const;

    /// Marks one entry as shown. Unknown ids are ignored, so a caller holding
    /// a copy of an entry that has since been dropped is not an error.
    void markSeen (uint64_t id);

    /// Marks everything currently recorded as shown. For the panel, which shows
    /// them all at once -- never for a single line, which can only have shown
    /// one of them.
    void markAllSeen();

    /// How many entries the user has not been shown. What a badge counts.
    size_t getUnseenCount() const;

    size_t size() const;
    void clear();

    /// How many entries the bound has dropped this session. A log that quietly
    /// loses its own beginning is the same failure this class exists to stop,
    /// one level up -- so the written log says so rather than simply starting
    /// part way through.
    size_t getDroppedCount() const;

    /// The journal as a JSON array, for session.json and the take folder's own
    /// copy. Oldest first there -- a log is read forwards.
    std::string toJson() const;

    /// The plain-language name for a level, as it appears in a written log.
    static const char* levelName (ActivityLevel level) noexcept;

private:
    mutable std::mutex lock;
    std::vector<ActivityEntry> entries; // oldest first
    uint64_t nextId = 1;
    size_t droppedCount = 0;

    void dropOldestLocked();
};

} // namespace mma
