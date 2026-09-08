#include "ActivityJournal.h"
#include <algorithm>
#include <cstdio>

namespace mma {

const char* ActivityJournal::levelName (ActivityLevel level) noexcept
{
    switch (level)
    {
        case ActivityLevel::Started:   return "started";
        case ActivityLevel::Stopped:   return "stopped";
        case ActivityLevel::Recovered: return "recovered";
        case ActivityLevel::Warning:   return "warning";
        case ActivityLevel::Failed:    return "failed";
    }

    return "started";
}

void ActivityJournal::note (double nowSeconds, ActivityLevel level,
                            std::string subject, std::string message)
{
    std::lock_guard<std::mutex> guard (lock);

    // A rig that is failing repeatedly says the same sentence over and over --
    // a mic on a bad cable can drop twice a second. Collapsing the repeat keeps
    // the journal readable without hiding anything: the count says it is still
    // happening, and the timestamp moves to the latest occurrence.
    if (! entries.empty())
    {
        auto& newest = entries.back();

        if (newest.level == level && newest.subject == subject && newest.message == message
            && nowSeconds - newest.atSeconds <= kRepeatWindowSeconds)
        {
            newest.atSeconds = nowSeconds;
            ++newest.repeats;

            // A repeat is news again: something the user was shown once and
            // dismissed is still going on, so it goes back to unseen.
            newest.seen = false;
            return;
        }
    }

    if (entries.size() >= kMaxEntries)
        dropOldestLocked();

    ActivityEntry entry;
    entry.id = nextId++;
    entry.atSeconds = nowSeconds;
    entry.level = level;
    entry.subject = std::move (subject);
    entry.message = std::move (message);
    entries.push_back (std::move (entry));
}

void ActivityJournal::dropOldestLocked()
{
    // The bound must never be the thing that makes a failure silent, which is
    // the whole point of this class. So the oldest *droppable* entry goes: an
    // unseen warning or failure is skipped over and something more ordinary is
    // dropped in its place. If every entry is an unseen failure -- a rig coming
    // apart -- the oldest one goes after all, because the alternative is
    // refusing to record the newest, and the newest is the one still true.
    const auto droppable = std::find_if (entries.begin(), entries.end(),
                                         [] (const ActivityEntry& e)
                                         {
                                             const bool serious = e.level == ActivityLevel::Warning
                                                               || e.level == ActivityLevel::Failed;
                                             return e.seen || ! serious;
                                         });

    entries.erase (droppable != entries.end() ? droppable : entries.begin());
    ++droppedCount;
}

std::vector<ActivityEntry> ActivityJournal::getEntries() const
{
    std::lock_guard<std::mutex> guard (lock);
    return { entries.rbegin(), entries.rend() };
}

bool ActivityJournal::getMostSeriousUnseen (ActivityEntry& out) const
{
    std::lock_guard<std::mutex> guard (lock);

    bool found = false;

    // Walked newest first so that ties land on the latest form of an ongoing
    // problem rather than on the first thing that ever went wrong.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        if (it->seen)
            continue;

        if (! found || it->level > out.level)
        {
            out = *it;
            found = true;
        }
    }

    return found;
}

void ActivityJournal::markSeen (uint64_t id)
{
    std::lock_guard<std::mutex> guard (lock);

    for (auto& e : entries)
    {
        if (e.id == id)
        {
            e.seen = true;
            return;
        }
    }
}

void ActivityJournal::markAllSeen()
{
    std::lock_guard<std::mutex> guard (lock);

    for (auto& e : entries)
        e.seen = true;
}

size_t ActivityJournal::getUnseenCount() const
{
    std::lock_guard<std::mutex> guard (lock);

    size_t count = 0;
    for (const auto& e : entries)
        if (! e.seen)
            ++count;

    return count;
}

size_t ActivityJournal::getDroppedCount() const
{
    std::lock_guard<std::mutex> guard (lock);
    return droppedCount;
}

size_t ActivityJournal::size() const
{
    std::lock_guard<std::mutex> guard (lock);
    return entries.size();
}

void ActivityJournal::clear()
{
    std::lock_guard<std::mutex> guard (lock);
    entries.clear();
    droppedCount = 0;
}

static void appendJsonString (std::string& out, const std::string& value)
{
    out += '"';

    for (const char c : value)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char> (c) < 0x20)
                {
                    // Control characters have no business in a message, but a
                    // device name comes from the OS and a malformed one must
                    // not be able to produce a session.json nothing can parse.
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(static_cast<unsigned char> (c) >> 4) & 0xf];
                    out += hex[static_cast<unsigned char> (c) & 0xf];
                }
                else
                {
                    out += c;
                }
        }
    }

    out += '"';
}

std::string ActivityJournal::toJson() const
{
    std::lock_guard<std::mutex> guard (lock);

    std::string out = "[";

    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto& e = entries[i];

        if (i > 0)
            out += ',';

        // Seconds to one decimal: enough to line an entry up against the take,
        // and short enough that the file stays readable by a person.
        char seconds[32];
        std::snprintf (seconds, sizeof (seconds), "%.1f", e.atSeconds);

        out += "{\"at\":";
        out += seconds;
        out += ",\"level\":";
        appendJsonString (out, levelName (e.level));
        out += ",\"subject\":";
        appendJsonString (out, e.subject);
        out += ",\"message\":";
        appendJsonString (out, e.message);
        out += ",\"repeats\":" + std::to_string (e.repeats);
        out += '}';
    }

    out += ']';
    return out;
}

} // namespace mma
