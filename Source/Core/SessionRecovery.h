#pragma once
#include "SessionMetadata.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mma {

/// One file found in an interrupted session folder, after its header has been
/// made to agree with the bytes actually on disk.
struct RecoveredFile
{
    std::string fileName;
    uint64_t frames = 0;
    double seconds = 0.0;
    /// True when the sizes in the header did not match the file, which is what
    /// a take killed between two header rewrites looks like.
    bool headerWasStale = false;
    /// §6.6: under a second of audio is an unplayable stub, not a recording.
    bool reportedEmpty = false;
};

/// An interrupted take: one whose session.json never got a stop timestamp.
struct RecoveredSession
{
    std::string folder;
    std::string startedIso;
    std::vector<RecoveredFile> files;

    /// Files with real audio in them -- what the user is actually being offered.
    int keptFileCount() const;
    /// Files that were there but held less than a second.
    int emptyFileCount() const;
    double longestSeconds() const;
    bool isWorthPresenting() const { return keptFileCount() > 0; }
};

/// §6.6 crash and power-loss recovery.
///
/// SessionWriter already rewrites each file's RIFF and data sizes every five
/// seconds precisely so that an interrupted file stays playable. Nothing ever
/// went looking for those files afterwards, so the guarantee was written and
/// never collected: after a force-quit or a power cut the audio was on the card
/// with a header describing a file up to five seconds shorter than it really
/// was, and the app came up as though nothing had happened.
///
/// The filesystem walk lives in the App layer. What is here is the part worth
/// being sure about, and it runs on a machine with no audio hardware.
class SessionRecovery
{
public:
    /// §6.6: "discard any recovered file containing under 1 second of audio;
    /// report it as empty rather than presenting an unplayable stub."
    static constexpr double kMinimumUsefulSeconds = 1.0;

    /// A take that stopped cleanly wrote a stop timestamp. One that did not is
    /// a take the app never got to finish.
    static bool sessionWasInterrupted (const SessionMetadata& meta);

    /// Rewrites the RIFF and data chunk sizes from the bytes actually present,
    /// and reports what the file turned out to hold. Safe to run on a file
    /// whose header is already correct: it reports headerWasStale = false and
    /// writes nothing.
    ///
    /// Returns frames == 0 for anything that is not a readable WAV, rather than
    /// throwing -- a folder recovered off a card can contain anything.
    static RecoveredFile repairWavFile (const std::string& path);
};

} // namespace mma
