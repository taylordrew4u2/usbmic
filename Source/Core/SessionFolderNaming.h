#pragma once
#include <string>
#include <functional>

namespace mma {

/// §6.2 session folder naming: `YYYY-MM-DD_HHMM_<name>`, sanitized (ASCII to
/// [A-Za-z0-9_-], letters in other scripts kept, NFC), whitespace collapsed to
/// single hyphens, truncated at 40 characters, and collision-avoided with _2,
/// _3, ... Never overwrite, never prompt.
class SessionFolderNaming
{
public:
    static constexpr size_t kMaxNameLength = 40;
    static constexpr const char* kDefaultName = "Session";

    /// Sanitize a user-provided session name (UTF-8): composed to NFC; ASCII
    /// kept only as [A-Za-z0-9_-], which leaves out / \ : * ? " < > |, dots
    /// and control characters; letters, digits and marks in other scripts
    /// kept, with invisible format/direction marks and C1 controls dropped;
    /// runs of any whitespace collapsed to a single hyphen (none leading);
    /// truncated to 40 characters, never mid-character. "Session" when
    /// nothing is left.
    static std::string sanitizeName (const std::string& rawName);

    /// sanitizeName for a name the user gives a microphone, input or camera,
    /// where empty means "no name of my own". Surrounding whitespace is
    /// trimmed, and a name with no letter or digit left in it (cleared, or
    /// only symbols like "!!!") comes back empty rather than as "Session" --
    /// so the caller clears its override instead of storing a name nobody typed.
    static std::string sanitizeNameOrEmpty (const std::string& rawName);

    /// A file name folded the way a case-insensitive volume (APFS and HFS+ by
    /// default, ExFAT, FAT, NTFS) compares it: ASCII, Latin-1, Latin
    /// Extended-A, Greek and Cyrillic capitals to lower case. For deciding
    /// whether two names would land on the same file -- now that names keep
    /// letters beyond ASCII, "Ü.json" and "ü.json" are one file on a Mac.
    static std::string foldCaseForComparison (const std::string& utf8Name);

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
