#pragma once
#include <string>
#include <functional>

namespace mma {

/// §6.2 session folder naming: `YYYY-MM-DD_HHMM_<name>`, sanitized to
/// [A-Za-z0-9 _-], whitespace collapsed to single hyphens, truncated at 40
/// chars, and collision-avoided with _2, _3, ... Never overwrite, never prompt.
class SessionFolderNaming
{
public:
    static constexpr size_t kMaxNameLength = 40;
    static constexpr const char* kDefaultName = "Session";

    /// Sanitize a user-provided session name: keep only [A-Za-z0-9 _-], collapse
    /// runs of whitespace to a single hyphen, truncate to 40 characters.
    static std::string sanitizeName (const std::string& rawName);

    /// Build "YYYY-MM-DD_HHMM_<name>" from calendar fields (already in local time).
    static std::string buildFolderName (int year, int month, int day, int hour, int minute,
                                        const std::string& sanitizedName);

    /// Given a desired folder name and a predicate that reports whether a name
    /// already exists (e.g. checks the filesystem), returns a name guaranteed not
    /// to collide, appending _2, _3, ... as needed. Never overwrites.
    static std::string resolveCollision (const std::string& desiredFolderName,
                                         const std::function<bool (const std::string&)>& exists);
};

} // namespace mma
